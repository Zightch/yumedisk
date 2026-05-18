package storer

import (
	"context"
	"errors"
	"fmt"
	"log"
	"net"
	"sync/atomic"

	"yumedisk/server/internal/config"
	"yumedisk/server/internal/gateway"
	"yumedisk/server/internal/route"
	"yumedisk/server/internal/transport"
)

type WholeRuntime struct {
	cfg      config.StorerConfig
	core     *Core
	gateway  *gateway.Handler
	routes   *route.Registry
	nextConn atomic.Uint64
}

func NewWholeRuntime(cfg config.StorerConfig, core *Core) (*WholeRuntime, error) {
	if cfg.Role != config.StorerRoleWhole {
		return nil, fmt.Errorf("whole runtime requires role=%q", config.StorerRoleWhole)
	}
	if core == nil {
		return nil, errors.New("whole runtime requires non-nil core")
	}

	routes := route.NewRegistry()
	if err := routes.Register(route.Entry{
		DiskID:        core.DiskID(),
		AuthVerifier:  core.AuthVerifier(),
		RouteTarget:   "embedded://whole",
		ConnectionID:  0,
		Connected:     true,
		DiskSizeBytes: core.DiskSize(),
		ReadOnly:      core.ReadOnly(),
		MaxIOBytes:    core.SessionService().MaxIOBytes(),
	}); err != nil {
		return nil, err
	}

	backend := newLocalGatewayBackend(core)
	gatewayHandler, err := gateway.NewHandler(routes, backend)
	if err != nil {
		return nil, err
	}

	return &WholeRuntime{
		cfg:     cfg,
		core:    core,
		gateway: gatewayHandler,
		routes:  routes,
	}, nil
}

func (r *WholeRuntime) ListenAddr() string {
	return r.cfg.Whole.ListenAddr
}

func (r *WholeRuntime) Run(ctx context.Context) error {
	listener, err := net.Listen("tcp", r.cfg.Whole.ListenAddr)
	if err != nil {
		return fmt.Errorf("listen on %s: %w", r.cfg.Whole.ListenAddr, err)
	}
	defer listener.Close()

	go func() {
		<-ctx.Done()
		_ = listener.Close()
	}()

	for {
		conn, err := listener.Accept()
		if err != nil {
			if ctx.Err() != nil {
				return nil
			}
			var netErr net.Error
			if errors.As(err, &netErr) && netErr.Temporary() {
				log.Printf("temporary accept error: %v", err)
				continue
			}
			return fmt.Errorf("accept connection: %w", err)
		}

		connectionID := r.nextConn.Add(1)
		state := r.gateway.NewConnectionState(connectionID)
		go r.serveAcceptedConnection(ctx, state, conn)
	}
}

func (r *WholeRuntime) serveAcceptedConnection(ctx context.Context, state *gateway.ConnectionState, conn net.Conn) {
	defer r.gateway.CloseConnection(state.ID)
	defer conn.Close()

	log.Printf("connection %d accepted from %s", state.ID, conn.RemoteAddr())

	runtime := transport.NewRuntime(conn, r.gateway.Bind(state))

	done := make(chan error, 1)
	go func() {
		done <- runtime.Run()
	}()

	select {
	case <-ctx.Done():
		_ = runtime.Close()
		<-done
	case err := <-done:
		if err != nil && !errors.Is(err, net.ErrClosed) {
			log.Printf("connection %d transport runtime: %v", state.ID, err)
		}
	}
}
