package connection

import (
	"context"
	"net"
	"testing"
	"time"

	"yumedisk/server/internal/bootstrap"
	"yumedisk/server/internal/proto"
	"yumedisk/server/internal/transport"
)

func TestServeAcceptedConnectionUsesProtocolErrorCloseReason(t *testing.T) {
	t.Parallel()

	serverConn, clientConn := net.Pipe()
	defer clientConn.Close()

	parent := &recordingParent{handleErr: ErrProtocolViolation}
	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()

	done := make(chan struct{})
	go func() {
		ServeAcceptedConnection(ctx, serverConn, parent, 7, Hooks{LogPrefix: "test"})
		close(done)
	}()

	if _, err := bootstrap.ConnectClient(clientConn); err != nil {
		t.Fatalf("hello bootstrap: %v", err)
	}
	if err := transport.WriteFrame(clientConn, []byte{1}); err != nil {
		t.Fatalf("write test payload: %v", err)
	}

	select {
	case <-done:
	case <-time.After(time.Second):
		t.Fatal("connection runtime did not stop in time")
	}
	if got := parent.closeReason(); got != proto.SessionCloseReasonProtocolError {
		t.Fatalf("unexpected close reason: %d", got)
	}
}

func TestServeAcceptedConnectionUsesGatewayShutdownReasonOnContextCancel(t *testing.T) {
	t.Parallel()

	serverConn, clientConn := net.Pipe()
	defer clientConn.Close()

	parent := &recordingParent{}
	ctx, cancel := context.WithCancel(context.Background())
	done := make(chan struct{})
	go func() {
		ServeAcceptedConnection(ctx, serverConn, parent, 8, Hooks{LogPrefix: "test"})
		close(done)
	}()

	if _, err := bootstrap.ConnectClient(clientConn); err != nil {
		t.Fatalf("hello bootstrap: %v", err)
	}
	cancel()

	select {
	case <-done:
	case <-time.After(time.Second):
		t.Fatal("connection runtime did not stop in time")
	}
	if got := parent.closeReason(); got != proto.SessionCloseReasonGatewayShutdown {
		t.Fatalf("unexpected close reason: %d", got)
	}
}

type recordingParent struct {
	handleErr error
	reasonCh  chan uint16
}

func (p *recordingParent) HandlePayload(*State, []byte) ([]byte, error) {
	return nil, p.handleErr
}

func (p *recordingParent) CloseConnectionWithReason(connectionID uint64, reason uint16) {
	if p.reasonCh == nil {
		p.reasonCh = make(chan uint16, 1)
	}
	p.reasonCh <- reason
}

func (p *recordingParent) closeReason() uint16 {
	if p.reasonCh == nil {
		return 0
	}
	select {
	case reason := <-p.reasonCh:
		return reason
	default:
		return 0
	}
}
