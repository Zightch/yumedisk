package storer

import (
	"context"
	"errors"
	"fmt"
	"net"
	"sync/atomic"

	"yumedisk/server/internal/config"
	"yumedisk/server/internal/gateway"
)

type WholeRuntime struct {
	cfg      config.StorerConfig
	gateway  *gateway.Handler
	nextConn atomic.Uint64
}

func NewWholeRuntime(cfg config.StorerConfig, core *Core) (*WholeRuntime, error) {
	if cfg.Role != config.StorerRoleWhole {
		return nil, fmt.Errorf("whole runtime requires role=%q", config.StorerRoleWhole)
	}
	if core == nil {
		return nil, errors.New("whole runtime requires non-nil core")
	}

	backend, err := newLocalGatewayBackend(core)
	if err != nil {
		return nil, err
	}
	gatewayHandler, err := gateway.NewHandler(backend, backend)
	if err != nil {
		return nil, err
	}

	return &WholeRuntime{
		cfg:     cfg,
		gateway: gatewayHandler,
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

	return gateway.ServeClientListener(
		ctx,
		listener,
		"client",
		func() uint64 { return r.nextConn.Add(1) },
		r.serveAcceptedConnection,
	)
}

func (r *WholeRuntime) serveAcceptedConnection(ctx context.Context, connectionID uint64, conn net.Conn) {
	gateway.ServeAcceptedClientConnection(ctx, conn, r.gateway, connectionID, gateway.ClientConnectionHooks{
		LogPrefix: "whole client",
	})
}
