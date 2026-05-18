package storer

import (
	"context"
	"fmt"
	"log"
	"net"
	"sync/atomic"

	"yumedisk/server/internal/config"
	"yumedisk/server/internal/proto"
	"yumedisk/server/internal/transport"
)

type Runtime interface {
	Run(ctx context.Context) error
}

type StorerRuntime struct {
	cfg      config.StorerConfig
	core     *Core
	nextConn atomic.Uint64
}

func NewStorerRuntime(cfg config.StorerConfig, core *Core) (*StorerRuntime, error) {
	if cfg.Role != config.StorerRoleStorer {
		return nil, fmt.Errorf("storer runtime requires role=%q", config.StorerRoleStorer)
	}
	if core == nil {
		return nil, fmt.Errorf("storer runtime requires non-nil core")
	}

	return &StorerRuntime{
		cfg:  cfg,
		core: core,
	}, nil
}

func (r *StorerRuntime) Run(ctx context.Context) error {
	conn, err := net.Dial("tcp", r.cfg.Storer.GatewayAddr)
	if err != nil {
		if ctx.Err() != nil {
			return nil
		}
		return fmt.Errorf("dial gateway %s: %w", r.cfg.Storer.GatewayAddr, err)
	}

	connectionID := r.nextConn.Add(1)
	handler := newDataPlaneHandler(connectionID, r.core.SessionService())
	runErr := r.runGatewayConnection(ctx, connectionID, conn, handler)
	if ctx.Err() != nil {
		return nil
	}
	if runErr != nil {
		log.Printf("storer gateway connection %d ended: %v", connectionID, runErr)
	}
	return runErr
}

func (r *StorerRuntime) runGatewayConnection(ctx context.Context, connectionID uint64, conn net.Conn, handler *dataPlaneHandler) error {
	defer r.core.SessionService().CloseConnection(connectionID)
	defer conn.Close()

	log.Printf("storer connected to gateway %s as connection %d", r.cfg.Storer.GatewayAddr, connectionID)

	registerReq := proto.BuildStorerRegisterRequestBody(proto.StorerRegisterRequest{
		GatewayToken:  r.cfg.Storer.GatewayToken,
		DiskID:        r.core.DiskID(),
		AuthVerifier:  r.core.AuthVerifier(),
		DiskSizeBytes: r.core.DiskSize(),
		ReadOnly:      r.core.ReadOnly(),
		MaxIOBytes:    r.core.SessionService().MaxIOBytes(),
	})
	registerPayload := make([]byte, proto.HeaderSize+len(registerReq))
	proto.EncodeHeader(proto.Header{
		ProtocolVersion: proto.ProtocolVersion,
		HeaderLen:       proto.HeaderSize,
		OpCode:          proto.OpStorerRegister,
		RequestID:       1,
	}, registerPayload)
	copy(registerPayload[proto.HeaderSize:], registerReq)
	if err := transport.WriteFrame(conn, registerPayload); err != nil {
		return fmt.Errorf("write register request: %w", err)
	}
	buffer := make([]byte, transport.MaxPayloadSize)
	registerResp, err := transport.ReadFrameInto(conn, buffer)
	if err != nil {
		return fmt.Errorf("read register response: %w", err)
	}
	registerHeader, err := proto.ParseHeader(registerResp)
	if err != nil {
		return fmt.Errorf("parse register response: %w", err)
	}
	if registerHeader.StatusCode != proto.StatusOK {
		return fmt.Errorf("register rejected: status=%d", registerHeader.StatusCode)
	}

	runtime := transport.NewRuntime(conn, handler)
	done := make(chan error, 1)
	go func() {
		done <- runtime.Run()
	}()

	select {
	case <-ctx.Done():
		_ = runtime.Close()
		<-done
		return nil
	case err := <-done:
		return err
	}
}

type RoleRuntime struct {
	cfg     config.StorerConfig
	core    *Core
	runtime Runtime
}

func NewRoleRuntime(cfg config.StorerConfig) (*RoleRuntime, error) {
	core, err := NewCore(cfg)
	if err != nil {
		return nil, err
	}

	runtime, err := newRuntimeForRole(cfg, core)
	if err != nil {
		_ = core.Close()
		return nil, err
	}

	return &RoleRuntime{
		cfg:     cfg,
		core:    core,
		runtime: runtime,
	}, nil
}

func (r *RoleRuntime) Close() error {
	if r == nil || r.core == nil {
		return nil
	}
	return r.core.Close()
}

func (r *RoleRuntime) Run(ctx context.Context) error {
	return r.runtime.Run(ctx)
}

func (r *RoleRuntime) Role() config.StorerRole {
	return r.cfg.Role
}

func (r *RoleRuntime) DiskID() string {
	return r.core.DiskID()
}

func (r *RoleRuntime) StoragePath() string {
	return r.core.StoragePath()
}

func (r *RoleRuntime) ListenAddr() string {
	if runtime, ok := r.runtime.(*WholeRuntime); ok {
		return runtime.ListenAddr()
	}
	return ""
}

func (r *RoleRuntime) GatewayAddr() string {
	if r.cfg.Role != config.StorerRoleStorer {
		return ""
	}
	return r.cfg.Storer.GatewayAddr
}

func newRuntimeForRole(cfg config.StorerConfig, core *Core) (Runtime, error) {
	switch cfg.Role {
	case config.StorerRoleWhole:
		return NewWholeRuntime(cfg, core)
	case config.StorerRoleStorer:
		return NewStorerRuntime(cfg, core)
	default:
		return nil, fmt.Errorf("unsupported role: %q", cfg.Role)
	}
}
