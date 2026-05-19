package storer

import (
	"context"
	"fmt"
	"log"
	"sync/atomic"
	"time"

	"yumedisk/server/internal/config"
	storegatewaylink "yumedisk/server/internal/storer/gateway/link"
)

type Runtime interface {
	Run(ctx context.Context) error
}

const storerLinkHeartbeatTimeout = 15 * time.Second

type StorerRuntime struct {
	linkRuntime *storegatewaylink.LinkRuntime
	nextConn    atomic.Uint64
}

func NewStorerRuntime(cfg config.StorerConfig, core *Core) (*StorerRuntime, error) {
	if cfg.Role != config.StorerRoleStorer {
		return nil, fmt.Errorf("storer runtime requires role=%q", config.StorerRoleStorer)
	}
	if core == nil {
		return nil, fmt.Errorf("storer runtime requires non-nil core")
	}

	linkRuntime, err := storegatewaylink.NewLinkRuntime(
		cfg.Storer.GatewayAddr,
		core.GatewayRegisterInfo(cfg.Storer.GatewayToken),
		core.SessionService(),
		storerLinkHeartbeatTimeout,
	)
	if err != nil {
		return nil, err
	}

	return &StorerRuntime{
		linkRuntime: linkRuntime,
	}, nil
}

func (r *StorerRuntime) Run(ctx context.Context) error {
	connectionID := r.nextConn.Add(1)
	runErr := r.linkRuntime.Run(ctx, connectionID)
	if ctx.Err() != nil {
		return nil
	}
	if runErr != nil {
		log.Printf("storer gateway connection %d ended: %v", connectionID, runErr)
	}
	return runErr
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
