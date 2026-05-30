package storer

import (
	"context"
	"errors"
	"fmt"
	"log"
	"net"
	"time"

	"yumedisk/server/internal/proto"
	"yumedisk/server/internal/transport"
)

type linkRuntime struct {
	registry          *Registry
	connection        *connection
	registerGate      *registerGate
	heartbeatInterval time.Duration
	heartbeatTimeout  time.Duration
}

func newLinkRuntime(
	connectionID uint64,
	conn net.Conn,
	registry *Registry,
	gatewayToken string,
	heartbeatInterval time.Duration,
	heartbeatTimeout time.Duration,
) *linkRuntime {
	return &linkRuntime{
		registry:          registry,
		connection:        registry.AttachConnection(connectionID, conn),
		registerGate:      newRegisterGate(registry.routes, gatewayToken),
		heartbeatInterval: heartbeatInterval,
		heartbeatTimeout:  heartbeatTimeout,
	}
}

func ServeListener(
	ctx context.Context,
	listener net.Listener,
	nextConnectionID func() uint64,
	registry *Registry,
	gatewayToken string,
	heartbeatInterval time.Duration,
	heartbeatTimeout time.Duration,
) error {
	if nextConnectionID == nil {
		return fmt.Errorf("storer listener requires connection id allocator")
	}
	if registry == nil {
		return fmt.Errorf("storer listener requires route registry")
	}

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

		runtime := newLinkRuntime(
			nextConnectionID(),
			conn,
			registry,
			gatewayToken,
			heartbeatInterval,
			heartbeatTimeout,
		)
		go func() {
			if err := runtime.Serve(ctx); err != nil && !errors.Is(err, net.ErrClosed) {
				log.Printf("gateway storer connection %d runtime: %v", runtime.connection.id, err)
			}
		}()
	}
}

func (r *linkRuntime) Serve(ctx context.Context) error {
	defer r.registry.DisconnectConnection(r.connection.id)
	defer r.connection.conn.Close()

	log.Printf("gateway storer connection %d accepted from %s", r.connection.id, r.connection.conn.RemoteAddr())

	r.connection.startHeartbeat(r.heartbeatInterval, r.heartbeatTimeout)
	done := make(chan error, 1)
	go func() {
		done <- r.serveFrames()
	}()

	select {
	case <-ctx.Done():
		_ = r.connection.conn.Close()
		<-done
		return nil
	case err := <-done:
		return err
	}
}

func (r *linkRuntime) serveFrames() error {
	buffer := make([]byte, transport.MaxPayloadSize)
	registered := false

	for {
		payload, err := transport.ReadFrameInto(r.connection.conn, buffer)
		if err != nil {
			if errors.Is(err, net.ErrClosed) {
				return nil
			}
			return err
		}

		header, err := proto.ParseHeader(payload)
		if err != nil {
			return err
		}

		if header.Flags&proto.FlagResponse == 0 {
			if header.Flags == proto.FlagNotice {
				if !registered {
					return fmt.Errorf("unexpected storer notice before register")
				}
				if err := proto.ValidateNoticeHeader(header); err != nil {
					return err
				}
				switch header.OpCode {
				case proto.OpSessionDataChangedNotice:
					if len(payload[proto.HeaderSize:]) != 0 {
						return fmt.Errorf("session data changed notice body must be empty")
					}
					r.registry.NotifyRouteSessionDataChanged(r.connection.id, header.SessionID)
					continue
				default:
					return fmt.Errorf("unsupported storer notice op: %d", header.OpCode)
				}
			}
			if registered {
				if err := transport.WriteFrame(r.connection.conn, proto.BuildErrorResponse(header, proto.StatusInvalidRequest)); err != nil {
					return err
				}
				continue
			}

			resp, accepted := r.registerGate.Handle(
				r.connection.id,
				r.connection.conn.RemoteAddr().String(),
				header,
				payload[proto.HeaderSize:],
			)
			if err := transport.WriteFrame(r.connection.conn, resp); err != nil {
				return err
			}
			if accepted {
				registered = true
				r.connection.markRegistered()
			}
			continue
		}

		if !registered {
			return fmt.Errorf("unexpected storer response before register")
		}
		r.connection.handleResponse(header, payload)
	}
}
