package storer

import (
	"net"
	"testing"
	"time"

	"yumedisk/server/internal/proto"
	"yumedisk/server/internal/transport"
)

func TestConnectionRoundTripTimesOutWithoutResponse(t *testing.T) {
	t.Parallel()

	serverConn, clientConn := net.Pipe()
	defer serverConn.Close()
	defer clientConn.Close()

	conn := newConnection(7, serverConn)
	payload := make([]byte, proto.HeaderSize+proto.LinkHeartbeatBodySize)
	proto.EncodeHeader(proto.Header{
		ProtocolVersion: proto.ProtocolVersion,
		HeaderLen:       proto.HeaderSize,
		OpCode:          proto.OpLinkHeartbeat,
		RequestID:       1,
	}, payload)
	copy(payload[proto.HeaderSize:], proto.BuildLinkHeartbeatBody(1))

	done := make(chan error, 1)
	go func() {
		_, err := conn.roundTripWithTimeout(payload, 20*time.Millisecond)
		done <- err
	}()

	buffer := make([]byte, transport.MaxPayloadSize)
	request, err := transport.ReadFrameInto(clientConn, buffer)
	if err != nil {
		t.Fatalf("read outbound request: %v", err)
	}
	header, err := proto.ParseHeader(request)
	if err != nil {
		t.Fatalf("parse outbound request: %v", err)
	}
	if header.OpCode != proto.OpLinkHeartbeat {
		t.Fatalf("unexpected outbound op: %d", header.OpCode)
	}

	select {
	case err := <-done:
		if err == nil {
			t.Fatal("expected timeout error")
		}
	case <-time.After(200 * time.Millisecond):
		t.Fatal("round trip did not time out")
	}
}
