package storer

import (
	"context"
	"errors"
	"fmt"
	"io"
	"log"
	"net"
	"sync/atomic"

	"yumedisk/server/internal/auth"
	"yumedisk/server/internal/config"
	"yumedisk/server/internal/gateway"
	"yumedisk/server/internal/session"
	filestorage "yumedisk/server/internal/storage/file"
	"yumedisk/server/internal/transport"
)

type Service struct {
	cfg      config.Config
	material auth.Material
	storage  *filestorage.Backend
	sessions *session.Manager
	gateway  *gateway.Handler
	nextConn atomic.Uint64
}

func NewService(cfg config.Config) (*Service, error) {
	material, err := auth.ParseClaimCode(cfg.ClaimCode)
	if err != nil {
		return nil, fmt.Errorf("parse claim code: %w", err)
	}

	storage, err := filestorage.Open(cfg.StorageFilePath, false)
	if err != nil {
		return nil, err
	}

	return &Service{
		cfg:      cfg,
		material: material,
		storage:  storage,
		sessions: session.NewManager(),
		gateway:  gateway.NewHandler(),
	}, nil
}

func (s *Service) ListenAddr() string {
	return s.cfg.ListenAddr
}

func (s *Service) DiskID() string {
	return s.material.DiskID
}

func (s *Service) StoragePath() string {
	return s.storage.Path()
}

func (s *Service) Run(ctx context.Context) error {
	listener, err := net.Listen("tcp", s.cfg.ListenAddr)
	if err != nil {
		return fmt.Errorf("listen on %s: %w", s.cfg.ListenAddr, err)
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
		go s.handleConnection(ctx, state, conn)
	}
}

func (s *Service) handleConnection(ctx context.Context, state *gateway.ConnectionState, conn net.Conn) {
	defer conn.Close()
	defer s.sessions.CloseConnection(state.ID)

	log.Printf("connection %d accepted from %s", state.ID, conn.RemoteAddr())

	buffer := make([]byte, transport.MaxPayloadSize)
	for {
		if ctx.Err() != nil {
			return
		}

		payload, err := transport.ReadFrameInto(conn, buffer)
		if err != nil {
			if errors.Is(err, io.EOF) || errors.Is(err, net.ErrClosed) {
				return
			}
			log.Printf("connection %d read frame: %v", state.ID, err)
			return
		}

		response, err := s.gateway.HandlePayload(state, payload)
		if err != nil {
			log.Printf("connection %d handle payload: %v", state.ID, err)
			return
		}
		if response == nil {
			continue
		}
		if err := transport.WriteFrame(conn, response); err != nil {
			log.Printf("connection %d write frame: %v", state.ID, err)
			return
		}
	}
}
