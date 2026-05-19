package client

import (
	"context"
	"net"

	connectionpkg "yumedisk/server/internal/gateway/client/connection"
)

type ConnectionState = connectionpkg.State
type ConnectionHandler = connectionpkg.Handler
type ClientConnectionHooks = connectionpkg.Hooks

func newConnectionHandler(parent connectionpkg.Parent, state *ConnectionState) *ConnectionHandler {
	return connectionpkg.NewHandler(parent, state)
}

func ServeAcceptedClientConnection(
	ctx context.Context,
	conn net.Conn,
	handler connectionpkg.Parent,
	connectionID uint64,
	hooks ClientConnectionHooks,
) {
	if hooks.WatchdogFactory == nil {
		hooks.WatchdogFactory = func() connectionpkg.Watchdog {
			return newClientHeartbeatWatchdog(clientHeartbeatTimeout)
		}
	}
	connectionpkg.ServeAcceptedConnection(ctx, conn, handler, connectionID, hooks)
}
