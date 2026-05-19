package gateway

import (
	"context"
	"fmt"
	"net"
	"sync"
	"sync/atomic"
	"time"

	"yumedisk/server/internal/config"
	storerruntime "yumedisk/server/internal/gateway/storer"
	"yumedisk/server/internal/proto"
	"yumedisk/server/internal/transport"
)

type Runtime struct {
	cfg           config.GatewayConfig
	clientHandler *Handler
	storerRoutes  *storerruntime.Registry
	nextConn      atomic.Uint64

	clientConnMu sync.RWMutex
	clientConns  map[uint64]*clientConnection
}

type clientConnection struct {
	conn  net.Conn
	write sync.Mutex
}

const (
	storerHeartbeatInterval = 5 * time.Second
	storerHeartbeatTimeout  = 15 * time.Second
)

func NewRuntime(cfg config.GatewayConfig) (*Runtime, error) {
	storerRoutes := storerruntime.NewRegistry()
	clientHandler, err := NewHandler(storerRoutes, storerRoutes)
	if err != nil {
		return nil, err
	}
	runtime := &Runtime{
		cfg:           cfg,
		clientHandler: clientHandler,
		storerRoutes:  storerRoutes,
		clientConns:   make(map[uint64]*clientConnection),
	}
	clientHandler.SetSessionCloseNotifier(runtime)
	storerRoutes.SetDisconnectHandler(clientHandler)
	return runtime, nil
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
		report(ServeClientListener(
			ctx,
			clientListener,
			"client",
			func() uint64 { return r.nextConn.Add(1) },
			r.serveClientConnection,
		))
	}()
	go func() {
		report(storerruntime.ServeListener(
			ctx,
			storerListener,
			func() uint64 { return r.nextConn.Add(1) },
			r.storerRoutes,
			r.cfg.Storer.GatewayToken,
			storerHeartbeatInterval,
			storerHeartbeatTimeout,
		))
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

func (r *Runtime) serveClientConnection(ctx context.Context, connectionID uint64, conn net.Conn) {
	ServeAcceptedClientConnection(ctx, conn, r.clientHandler, connectionID, ClientConnectionHooks{
		LogPrefix: "gateway client",
		OnConnected: func(conn net.Conn, state *ConnectionState) {
			r.clientConnMu.Lock()
			r.clientConns[state.ID] = &clientConnection{
				conn: conn,
			}
			r.clientConnMu.Unlock()
		},
		OnClosed: func(state *ConnectionState) {
			r.clientConnMu.Lock()
			delete(r.clientConns, state.ID)
			r.clientConnMu.Unlock()
		},
	})
}

func (r *Runtime) NotifySessionClosed(session gatewaySessionRecord, reason uint16) {
	r.clientConnMu.RLock()
	client := r.clientConns[session.Runtime.ClientConnectionID]
	r.clientConnMu.RUnlock()
	if client == nil {
		return
	}

	payload := make([]byte, proto.HeaderSize+proto.SessionCloseNoticeSize)
	copy(payload, proto.BuildNotice(proto.OpSessionCloseNotice, session.ID, proto.BuildSessionCloseNoticeBody(reason)))

	client.write.Lock()
	err := transport.WriteFrame(client.conn, payload)
	client.write.Unlock()
	if err != nil {
		_ = client.conn.Close()
		return
	}
}
