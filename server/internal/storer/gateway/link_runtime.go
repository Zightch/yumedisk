package gateway

import (
	"context"
	"fmt"
	"log"
	"net"
	"time"

	"yumedisk/server/internal/session"
	"yumedisk/server/internal/transport"
)

type LinkRuntime struct {
	gatewayAddr      string
	registerClient   *RegisterClient
	sessions         *session.Service
	heartbeatTimeout time.Duration
}

func NewLinkRuntime(
	gatewayAddr string,
	registerInfo RegisterInfo,
	sessions *session.Service,
	heartbeatTimeout time.Duration,
) (*LinkRuntime, error) {
	if gatewayAddr == "" {
		return nil, fmt.Errorf("gateway link runtime requires gateway address")
	}
	if sessions == nil {
		return nil, fmt.Errorf("gateway link runtime requires session service")
	}

	return &LinkRuntime{
		gatewayAddr:      gatewayAddr,
		registerClient:   NewRegisterClient(registerInfo),
		sessions:         sessions,
		heartbeatTimeout: heartbeatTimeout,
	}, nil
}

func (r *LinkRuntime) Run(ctx context.Context, connectionID uint64) error {
	var dialer net.Dialer
	conn, err := dialer.DialContext(ctx, "tcp", r.gatewayAddr)
	if err != nil {
		if ctx.Err() != nil {
			return nil
		}
		return fmt.Errorf("dial gateway %s: %w", r.gatewayAddr, err)
	}
	defer r.sessions.CloseConnection(connectionID)
	defer conn.Close()

	log.Printf("storer connected to gateway %s as connection %d", r.gatewayAddr, connectionID)

	if err := r.registerClient.Register(conn); err != nil {
		return err
	}

	watchdog := newLinkHeartbeatWatchdog(r.heartbeatTimeout)
	handler := newDataPlaneHandler(connectionID, r.sessions, watchdog)
	watchdog.Mark()
	watchdogErr := watchdog.Start(conn)
	defer watchdog.Stop()

	runtime := transport.NewRuntime(conn, handler)
	done := make(chan error, 1)
	go func() {
		done <- runtime.Run()
	}()

	select {
	case <-ctx.Done():
		_ = runtime.Close()
		<-done
		return nil
	case err := <-watchdogErr:
		_ = runtime.Close()
		<-done
		return err
	case err := <-done:
		return err
	}
}
