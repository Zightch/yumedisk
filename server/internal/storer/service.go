package storer

import (
	"context"
	"errors"
	"fmt"
	"log"
	"net"
	"sync/atomic"
	"time"

	"yumedisk/server/internal/auth"
	"yumedisk/server/internal/config"
	"yumedisk/server/internal/gateway"
	"yumedisk/server/internal/session"
	filestorage "yumedisk/server/internal/storage/file"
	"yumedisk/server/internal/transport"
)

type Service struct {
	cfg      config.StorerConfig
	material auth.Material
	storage  *filestorage.Backend
	sessions *session.Manager
	gateway  *gateway.Handler
	nextConn atomic.Uint64
}

func NewService(cfg config.StorerConfig) (*Service, error) {
	if cfg.Role != config.StorerRoleWhole {
		return nil, fmt.Errorf("embedded gateway service only supports role=%q", config.StorerRoleWhole)
	}

	material, err := auth.ParseClaimCode(cfg.ClaimCode)
	if err != nil {
		return nil, fmt.Errorf("parse claim code: %w", err)
	}

	storage, err := filestorage.Open(cfg.StorageFilePath, false)
	if err != nil {
		return nil, err
	}

	sessions := session.NewService(session.NewManager(), storage, 30*time.Second, 60*1024)
	gatewayHandler, err := gateway.NewHandler(material.DiskID, material.AuthVerifier, sessions)
	if err != nil {
		return nil, err
	}

	return &Service{
		cfg:      cfg,
		material: material,
		storage:  storage,
		sessions: sessions.Manager(),
		gateway:  gatewayHandler,
	}, nil
}

func (s *Service) Close() error {
	if s.storage == nil {
		return nil
	}
	return s.storage.Close()
}

func (s *Service) ListenAddr() string {
	return s.cfg.Whole.ListenAddr
}

func (s *Service) DiskID() string {
	return s.material.DiskID
}

func (s *Service) StoragePath() string {
	return s.storage.Path()
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
	defer s.sessions.CloseConnection(state.ID)
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
