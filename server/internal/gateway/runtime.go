package gateway

import (
	"context"
	"errors"
	"fmt"
	"log"
	"net"
	"sync"
	"sync/atomic"

	"yumedisk/server/internal/config"
	"yumedisk/server/internal/session"
	"yumedisk/server/internal/transport"
)

type Runtime struct {
	cfg           config.GatewayConfig
	clientHandler *Handler
	storerHandler *StorerHandler
	routes        *StorerRouteRegistry
	nextConn      atomic.Uint64
}

func NewRuntime(cfg config.GatewayConfig) (*Runtime, error) {
	routes := NewStorerRouteRegistry()
	clientHandler, err := NewHandler(routes, newUnavailableDataPlane())
	if err != nil {
		return nil, err
	}
	storerHandler, err := NewStorerHandler(routes)
	if err != nil {
		return nil, err
	}

	return &Runtime{
		cfg:           cfg,
		clientHandler: clientHandler,
		storerHandler: storerHandler,
		routes:        routes,
	}, nil
}

func (r *Runtime) Run(ctx context.Context) error {
	clientListener, err := net.Listen("tcp", r.cfg.Client.ListenAddr)
	if err != nil {
		return fmt.Errorf("listen client on %s: %w", r.cfg.Client.ListenAddr, err)
	}
	defer clientListener.Close()

	storerListener, err := net.Listen("tcp", r.cfg.Storer.ListenAddr)
	if err != nil {
		return fmt.Errorf("listen storer on %s: %w", r.cfg.Storer.ListenAddr, err)
	}
	defer storerListener.Close()

	go func() {
		<-ctx.Done()
		_ = clientListener.Close()
		_ = storerListener.Close()
	}()

	errCh := make(chan error, 2)
	var once sync.Once
	report := func(err error) {
		if err == nil {
			return
		}
		once.Do(func() {
			errCh <- err
		})
	}

	go func() {
		report(r.serveClientListener(ctx, clientListener))
	}()
	go func() {
		report(r.serveStorerListener(ctx, storerListener))
	}()

	select {
	case <-ctx.Done():
		return nil
	case err := <-errCh:
		return err
	}
}

func (r *Runtime) ClientListenAddr() string {
	return r.cfg.Client.ListenAddr
}

func (r *Runtime) StorerListenAddr() string {
	return r.cfg.Storer.ListenAddr
}

func (r *Runtime) serveClientListener(ctx context.Context, listener net.Listener) error {
	for {
		conn, err := listener.Accept()
		if err != nil {
			if ctx.Err() != nil {
				return nil
			}
			var netErr net.Error
			if errors.As(err, &netErr) && netErr.Temporary() {
				log.Printf("temporary client accept error: %v", err)
				continue
			}
			return fmt.Errorf("accept client connection: %w", err)
		}

		connectionID := r.nextConn.Add(1)
		state := r.clientHandler.NewConnectionState(connectionID)
		go r.serveClientConnection(ctx, state, conn)
	}
}

func (r *Runtime) serveStorerListener(ctx context.Context, listener net.Listener) error {
	for {
		conn, err := listener.Accept()
		if err != nil {
			if ctx.Err() != nil {
				return nil
			}
			var netErr net.Error
			if errors.As(err, &netErr) && netErr.Temporary() {
				log.Printf("temporary storer accept error: %v", err)
				continue
			}
			return fmt.Errorf("accept storer connection: %w", err)
		}

		connectionID := r.nextConn.Add(1)
		go r.serveStorerConnection(ctx, connectionID, conn)
	}
}

func (r *Runtime) serveClientConnection(ctx context.Context, state *ConnectionState, conn net.Conn) {
	defer r.clientHandler.CloseConnection(state.ID)
	defer conn.Close()

	log.Printf("gateway client connection %d accepted from %s", state.ID, conn.RemoteAddr())

	runtime := transport.NewRuntime(conn, r.clientHandler.Bind(state))
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
			log.Printf("gateway client connection %d runtime: %v", state.ID, err)
		}
	}
}

func (r *Runtime) serveStorerConnection(ctx context.Context, connectionID uint64, conn net.Conn) {
	defer r.routes.DisconnectConnection(connectionID)
	defer conn.Close()

	log.Printf("gateway storer connection %d accepted from %s", connectionID, conn.RemoteAddr())

	runtime := transport.NewRuntime(conn, r.storerHandler)
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
			log.Printf("gateway storer connection %d runtime: %v", connectionID, err)
		}
	}
}

type unavailableDataPlane struct{}

func newUnavailableDataPlane() *unavailableDataPlane {
	return &unavailableDataPlane{}
}

func (p *unavailableDataPlane) Open(uint64, string) (session.Descriptor, error) {
	return session.Descriptor{}, session.ErrSessionUnavailable
}

func (p *unavailableDataPlane) Ping(uint64) (session.Descriptor, bool) {
	return session.Descriptor{}, false
}

func (p *unavailableDataPlane) Close(uint64) {}

func (p *unavailableDataPlane) CloseConnection(uint64) {}

func (p *unavailableDataPlane) Read(uint64, uint64, uint32) ([]byte, error) {
	return nil, session.ErrSessionUnavailable
}

func (p *unavailableDataPlane) Write(uint64, uint64, []byte) error {
	return session.ErrSessionUnavailable
}

func (p *unavailableDataPlane) TTLSeconds() uint32 {
	return 0
}
