package client

import (
	"context"
	"errors"
	"io"
	"log"
	"net"
	"time"

	"yumedisk/server/internal/bootstrap"
	"yumedisk/server/internal/transport"
)

const clientHeartbeatTimeout = 15 * time.Second

type ClientConnectionHooks struct {
	LogPrefix   string
	OnConnected func(conn net.Conn, state *ConnectionState)
	OnClosed    func(state *ConnectionState)
}

func ServeAcceptedClientConnection(
	ctx context.Context,
	conn net.Conn,
	handler *Handler,
	connectionID uint64,
	hooks ClientConnectionHooks,
) {
	logPrefix := hooks.LogPrefix
	if logPrefix == "" {
		logPrefix = "client"
	}

	defer conn.Close()
	if err := bootstrap.AcceptClient(conn); err != nil {
		if !errors.Is(err, io.EOF) && !errors.Is(err, net.ErrClosed) {
			log.Printf("%s bootstrap rejected from %s: %v", logPrefix, conn.RemoteAddr(), err)
		}
		return
	}

	state := handler.NewConnectionState(connectionID)
	watchdog := newClientHeartbeatWatchdog(clientHeartbeatTimeout)
	state.setHeartbeatWatchdog(watchdog)
	watchdog.Mark()

	if hooks.OnConnected != nil {
		hooks.OnConnected(conn, state)
	}
	defer handler.CloseConnection(state.ID)
	if hooks.OnClosed != nil {
		defer hooks.OnClosed(state)
	}

	log.Printf("%s connection %d accepted from %s", logPrefix, state.ID, conn.RemoteAddr())

	runtime := transport.NewRuntime(conn, handler.Bind(state))
	watchdogErr := watchdog.Start(conn)
	defer watchdog.Stop()

	done := make(chan error, 1)
	go func() {
		done <- runtime.Run()
	}()

	select {
	case <-ctx.Done():
		_ = runtime.Close()
		<-done
	case err := <-watchdogErr:
		_ = runtime.Close()
		<-done
		if err != nil && !errors.Is(err, net.ErrClosed) {
			log.Printf("%s connection %d heartbeat timeout: %v", logPrefix, state.ID, err)
		}
	case err := <-done:
		if err != nil && !errors.Is(err, net.ErrClosed) {
			log.Printf("%s connection %d runtime: %v", logPrefix, state.ID, err)
		}
	}
}
