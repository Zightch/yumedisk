package gateway

import (
	"net"
	"testing"

	"yumedisk/server/internal/proto"
	"yumedisk/server/internal/transport"
)

func TestStorerHandlerReturnsExplicitNotImplemented(t *testing.T) {
	t.Parallel()

	handler, err := NewStorerHandler(NewStorerRouteRegistry())
	if err != nil {
		t.Fatalf("new storer handler: %v", err)
	}

	serverConn, clientConn := net.Pipe()
	defer clientConn.Close()
	defer serverConn.Close()

	storerConn := newStorerConnection(7, serverConn)

	done := make(chan error, 1)
	go func() {
		done <- handler.ServeConnection(storerConn, "expected-token")
	}()

	reqBody := proto.BuildStorerRegisterRequestBody(proto.StorerRegisterRequest{
		GatewayToken:      "wrong-token",
		DiskID:            "DISK000000000001",
		AuthVerifier:      [64]byte{1},
		DiskSizeBytes:     4096,
		MaxIOBytes:        1024,
		SessionTTLSeconds: 30,
	})
	req := make([]byte, proto.HeaderSize+len(reqBody))
	proto.EncodeHeader(proto.Header{
		ProtocolVersion: proto.ProtocolVersion,
		HeaderLen:       proto.HeaderSize,
		OpCode:          proto.OpStorerRegister,
		RequestID:       1,
	}, req)
	copy(req[proto.HeaderSize:], reqBody)
	if err := transport.WriteFrame(clientConn, req); err != nil {
		t.Fatalf("write register request: %v", err)
	}

	buffer := make([]byte, transport.MaxPayloadSize)
	resp, err := transport.ReadFrameInto(clientConn, buffer)
	if err != nil {
		t.Fatalf("read register response: %v", err)
	}
	header, err := proto.ParseHeader(resp)
	if err != nil {
		t.Fatalf("parse register response: %v", err)
	}
	if header.StatusCode != proto.StatusAuthFailed {
		t.Fatalf("unexpected register status: %d", header.StatusCode)
	}

	_ = clientConn.Close()
	_ = serverConn.Close()
}
