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

func TestStorerHandlerRejectsDuplicateDiskOnDifferentConnection(t *testing.T) {
	t.Parallel()

	registry := NewStorerRouteRegistry()
	handler, err := NewStorerHandler(registry)
	if err != nil {
		t.Fatalf("new storer handler: %v", err)
	}

	const gatewayToken = "expected-token"
	const diskID = "DISK000000000001"

	serverConnOne, clientConnOne := net.Pipe()
	defer clientConnOne.Close()
	defer serverConnOne.Close()
	storerConnOne := newStorerConnection(7, serverConnOne)
	doneOne := make(chan error, 1)
	go func() {
		doneOne <- handler.ServeConnection(storerConnOne, gatewayToken)
	}()

	registerAndExpectStatus(t, clientConnOne, gatewayToken, diskID, proto.StatusOK)

	serverConnTwo, clientConnTwo := net.Pipe()
	defer clientConnTwo.Close()
	defer serverConnTwo.Close()
	storerConnTwo := newStorerConnection(8, serverConnTwo)
	doneTwo := make(chan error, 1)
	go func() {
		doneTwo <- handler.ServeConnection(storerConnTwo, gatewayToken)
	}()

	registerAndExpectStatus(t, clientConnTwo, gatewayToken, diskID, proto.StatusInvalidRequest)

	_ = clientConnTwo.Close()
	_ = clientConnOne.Close()
	_ = serverConnTwo.Close()
	_ = serverConnOne.Close()

	<-doneTwo
	<-doneOne
}

func TestStorerHandlerRejectsWrongPhaseRequests(t *testing.T) {
	t.Parallel()

	handler, err := NewStorerHandler(NewStorerRouteRegistry())
	if err != nil {
		t.Fatalf("new storer handler: %v", err)
	}

	serverConn, clientConn := net.Pipe()
	defer clientConn.Close()
	defer serverConn.Close()

	storerConn := newStorerConnection(9, serverConn)
	done := make(chan error, 1)
	go func() {
		done <- handler.ServeConnection(storerConn, "expected-token")
	}()

	pingReq := make([]byte, proto.HeaderSize+8)
	proto.EncodeHeader(proto.Header{
		ProtocolVersion: proto.ProtocolVersion,
		HeaderLen:       proto.HeaderSize,
		OpCode:          proto.OpPing,
		RequestID:       1,
		SessionID:       1,
	}, pingReq)
	copy(pingReq[proto.HeaderSize:], proto.BuildPingResponseBody(1))
	if err := transport.WriteFrame(clientConn, pingReq); err != nil {
		t.Fatalf("write pre-register ping: %v", err)
	}

	buffer := make([]byte, transport.MaxPayloadSize)
	pingResp, err := transport.ReadFrameInto(clientConn, buffer)
	if err != nil {
		t.Fatalf("read pre-register ping response: %v", err)
	}
	pingHeader, err := proto.ParseHeader(pingResp)
	if err != nil {
		t.Fatalf("parse pre-register ping response: %v", err)
	}
	if pingHeader.StatusCode != proto.StatusInvalidRequest {
		t.Fatalf("unexpected pre-register ping status: %d", pingHeader.StatusCode)
	}

	registerAndExpectStatus(t, clientConn, "expected-token", "DISK000000000001", proto.StatusOK)
	registerAndExpectStatus(t, clientConn, "expected-token", "DISK000000000001", proto.StatusInvalidRequest)

	_ = clientConn.Close()
	_ = serverConn.Close()
	<-done
}

func registerAndExpectStatus(t *testing.T, conn net.Conn, gatewayToken string, diskID string, want uint16) {
	t.Helper()

	reqBody := proto.BuildStorerRegisterRequestBody(proto.StorerRegisterRequest{
		GatewayToken:      gatewayToken,
		DiskID:            diskID,
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
	if err := transport.WriteFrame(conn, req); err != nil {
		t.Fatalf("write register request: %v", err)
	}

	buffer := make([]byte, transport.MaxPayloadSize)
	resp, err := transport.ReadFrameInto(conn, buffer)
	if err != nil {
		t.Fatalf("read register response: %v", err)
	}
	header, err := proto.ParseHeader(resp)
	if err != nil {
		t.Fatalf("parse register response: %v", err)
	}
	if header.StatusCode != want {
		t.Fatalf("unexpected register status: got=%d want=%d", header.StatusCode, want)
	}
}
