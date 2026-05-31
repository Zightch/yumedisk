package storer

import (
	"context"
	"errors"
	"fmt"
	"net"
	"sync"
	"sync/atomic"

	"yumedisk/server/internal/config"
	gatewayclient "yumedisk/server/internal/gateway/client"
	"yumedisk/server/internal/proto"
	storegateway "yumedisk/server/internal/storer/gateway"
	"yumedisk/server/internal/transport"
)

type WholeRuntime struct {
	cfg      config.StorerConfig
	gateway  *gatewayclient.Handler
	nextConn atomic.Uint64

	clientConnMu sync.RWMutex
	clientConns  map[uint64]*wholeClientConnection
}

type wholeClientConnection struct {
	conn  net.Conn
	write sync.Mutex
}

func NewWholeRuntime(cfg config.StorerConfig, core *Core) (*WholeRuntime, error) {
	if cfg.Role != config.StorerRoleWhole {
		return nil, fmt.Errorf("whole runtime requires role=%q", config.StorerRoleWhole)
	}
	if core == nil {
		return nil, errors.New("whole runtime requires non-nil core")
	}

	exports := make([]storegateway.LocalExport, 0, len(core.ExportIDs()))
	for _, exportID := range core.ExportIDs() {
		export, ok := core.Export(exportID)
		if !ok {
			return nil, fmt.Errorf("whole runtime missing export %q", exportID)
		}
		exports = append(exports, export)
	}

	backend, err := storegateway.NewLocalAdapter(exports)
	if err != nil {
		return nil, err
	}
	gatewayHandler, err := gatewayclient.NewHandler(backend, backend)
	if err != nil {
		return nil, err
	}
	backend.SetDataChangedHandler(gatewayHandler)
	core.SetDataChangedNotifier(backend)

	runtime := &WholeRuntime{
		cfg:         cfg,
		gateway:     gatewayHandler,
		clientConns: make(map[uint64]*wholeClientConnection),
	}
	gatewayHandler.SetSessionDataChangedNotifier(runtime)
	gatewayHandler.SetSessionCloseNotifier(runtime)
	return runtime, nil
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

	return gatewayclient.ServeClientListener(
		ctx,
		listener,
		"client",
		func() uint64 { return r.nextConn.Add(1) },
		r.serveAcceptedConnection,
	)
}

func (r *WholeRuntime) serveAcceptedConnection(ctx context.Context, connectionID uint64, conn net.Conn) {
	gatewayclient.ServeAcceptedClientConnection(ctx, conn, r.gateway, connectionID, gatewayclient.ClientConnectionHooks{
		LogPrefix: "whole client",
		OnConnected: func(conn net.Conn, state *gatewayclient.ConnectionState) {
			r.clientConnMu.Lock()
			r.clientConns[state.ID] = &wholeClientConnection{conn: conn}
			r.clientConnMu.Unlock()
		},
		OnClosed: func(state *gatewayclient.ConnectionState) {
			r.clientConnMu.Lock()
			delete(r.clientConns, state.ID)
			r.clientConnMu.Unlock()
		},
	})
}

func (r *WholeRuntime) NotifySessionDataChanged(sessionID uint64, clientConnectionID uint64) {
	r.clientConnMu.RLock()
	client := r.clientConns[clientConnectionID]
	r.clientConnMu.RUnlock()
	if client == nil {
		return
	}

	payload := proto.BuildNotice(proto.OpSessionDataChangedNotice, sessionID, nil)

	client.write.Lock()
	err := transport.WriteFrame(client.conn, payload)
	client.write.Unlock()
	if err != nil {
		_ = client.conn.Close()
	}
}

func (r *WholeRuntime) NotifySessionClosed(sessionID uint64, clientConnectionID uint64, body []byte) {
	r.clientConnMu.RLock()
	client := r.clientConns[clientConnectionID]
	r.clientConnMu.RUnlock()
	if client == nil {
		return
	}

	payload := proto.BuildNotice(proto.OpSessionCloseNotice, sessionID, body)

	client.write.Lock()
	err := transport.WriteFrame(client.conn, payload)
	client.write.Unlock()
	if err != nil {
		_ = client.conn.Close()
	}
}
