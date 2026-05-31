package connection

import (
	"context"
	"errors"
	"io"
	"log"
	"net"

	"yumedisk/server/internal/bootstrap"
	"yumedisk/server/internal/proto"
	"yumedisk/server/internal/transport"
)

var ErrProtocolViolation = errors.New("protocol violation")

type Parent interface {
	HandlePayload(state *State, payload []byte) ([]byte, error)
	CloseConnectionWithReason(connectionID uint64, reason uint16)
}

type Watchdog interface {
	HeartbeatMarker
	Start(conn net.Conn) <-chan error
	Stop()
}

type Handler struct {
	parent Parent
	state  *State
}

type Hooks struct {
	LogPrefix       string
	OnConnected     func(conn net.Conn, state *State)
	OnClosed        func(state *State)
	WatchdogFactory func() Watchdog
}

func NewHandler(parent Parent, state *State) *Handler {
	return &Handler{
		parent: parent,
		state:  state,
	}
}

func (h *Handler) HandlePayload(payload []byte) ([]byte, error) {
	return h.parent.HandlePayload(h.state, payload)
}

func ServeAcceptedConnection(
	ctx context.Context,
	conn net.Conn,
	handler Parent,
	connectionID uint64,
	hooks Hooks,
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

	state := &State{ID: connectionID}
	if hooks.OnConnected != nil {
		hooks.OnConnected(conn, state)
	}

	var watchdog Watchdog
	var watchdogErr <-chan error
	if hooks.WatchdogFactory != nil {
		watchdog = hooks.WatchdogFactory()
		state.SetHeartbeatMarker(watchdog)
		watchdog.Mark()
		watchdogErr = watchdog.Start(conn)
		defer watchdog.Stop()
	}

	closeReason := proto.SessionCloseReasonNormalClose
	defer func() {
		handler.CloseConnectionWithReason(state.ID, closeReason)
	}()
	if hooks.OnClosed != nil {
		defer hooks.OnClosed(state)
	}

	log.Printf("%s connection %d accepted from %s", logPrefix, state.ID, conn.RemoteAddr())

	// The transport runtime owns packet framing for this TCP connection; the
	// higher-level connection state and business protocol run on top of it.
	runtime := transport.NewRuntime(conn, NewHandler(handler, state))
	done := make(chan error, 1)
	go func() {
		done <- runtime.Run()
	}()

	select {
	case <-ctx.Done():
		closeReason = proto.SessionCloseReasonGatewayShutdown
		_ = runtime.Close()
		<-done
	case err := <-watchdogErr:
		_ = runtime.Close()
		<-done
		if err != nil && !errors.Is(err, net.ErrClosed) {
			log.Printf("%s connection %d heartbeat timeout: %v", logPrefix, state.ID, err)
		}
	case err := <-done:
		if isProtocolViolation(err) {
			closeReason = proto.SessionCloseReasonProtocolError
		}
		if err != nil && !errors.Is(err, net.ErrClosed) {
			log.Printf("%s connection %d runtime: %v", logPrefix, state.ID, err)
		}
	}
}

func isProtocolViolation(err error) bool {
	return errors.Is(err, ErrProtocolViolation) ||
		errors.Is(err, transport.ErrPayloadOutOfRange) ||
		errors.Is(err, transport.ErrBufferTooSmall)
}
