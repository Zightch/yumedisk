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
	"yumedisk/server/internal/transport"
)

type Service struct {
	cfg      config.StorerConfig
	core     *Core
	gateway  *gateway.Handler
	nextConn atomic.Uint64
}

func NewService(cfg config.StorerConfig) (*Service, error) {
	if cfg.Role != config.StorerRoleWhole {
		return nil, fmt.Errorf("embedded gateway service only supports role=%q", config.StorerRoleWhole)
	}

	core, err := NewCore(cfg)
	if err != nil {
		return nil, err
	}

	gatewayHandler, err := gateway.NewHandler(core.DiskID(), core.AuthVerifier(), core.SessionService())
	if err != nil {
		_ = core.Close()
		return nil, err
	}

	return &Service{
		cfg:     cfg,
		core:    core,
		gateway: gatewayHandler,
	}, nil
}

func (s *Service) Close() error {
	return s.core.Close()
}

func (s *Service) ListenAddr() string {
	return s.cfg.Whole.ListenAddr
}

func (s *Service) DiskID() string {
	return s.core.DiskID()
}

func (s *Service) StoragePath() string {
	return s.core.StoragePath()
}

func (s *Service) Run(ctx context.Context) error {
	listener, err := net.Listen("tcp", s.cfg.Whole.ListenAddr)
	if err != nil {
		return fmt.Errorf("listen on %s: %w", s.cfg.Whole.ListenAddr, err)
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

		connectionID := s.nextConn.Add(1)
		state := s.gateway.NewConnectionState(connectionID)
		go s.serveAcceptedConnection(ctx, state, conn)
	}
}

func (s *Service) serveAcceptedConnection(ctx context.Context, state *gateway.ConnectionState, conn net.Conn) {
	defer s.core.SessionService().CloseConnection(state.ID)
	defer conn.Close()

	log.Printf("connection %d accepted from %s", state.ID, conn.RemoteAddr())

	runtime := transport.NewRuntime(conn, s.gateway.Bind(state))

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
