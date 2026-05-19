package storer

import (
	"context"
	"net"
	"testing"
	"time"

	"yumedisk/server/internal/proto"
	"yumedisk/server/internal/transport"
)

func TestLinkRuntimeRejectsWrongGatewayToken(t *testing.T) {
	t.Parallel()

	serverConn, clientConn := net.Pipe()
	defer clientConn.Close()
	defer serverConn.Close()

	runtime := newLinkRuntime(7, serverConn, NewRegistry(), "expected-token", time.Hour, time.Hour)
	done := make(chan error, 1)
	go func() {
		done <- runtime.Serve(context.Background())
	}()

	registerAndExpectStatus(t, clientConn, "wrong-token", "DISK000000000001", proto.StatusAuthFailed)
	_ = clientConn.Close()
	_ = serverConn.Close()
	<-done
}

func TestLinkRuntimeRejectsDuplicateDiskOnDifferentConnection(t *testing.T) {
	t.Parallel()

	registry := NewRegistry()
	const gatewayToken = "expected-token"
	const diskID = "DISK000000000001"

	serverConnOne, clientConnOne := net.Pipe()
	defer clientConnOne.Close()
	defer serverConnOne.Close()
	runtimeOne := newLinkRuntime(7, serverConnOne, registry, gatewayToken, time.Hour, time.Hour)
	doneOne := make(chan error, 1)
	go func() {
		doneOne <- runtimeOne.Serve(context.Background())
	}()

	registerAndExpectStatus(t, clientConnOne, gatewayToken, diskID, proto.StatusOK)

	serverConnTwo, clientConnTwo := net.Pipe()
	defer clientConnTwo.Close()
	defer serverConnTwo.Close()
	runtimeTwo := newLinkRuntime(8, serverConnTwo, registry, gatewayToken, time.Hour, time.Hour)
	doneTwo := make(chan error, 1)
	go func() {
		doneTwo <- runtimeTwo.Serve(context.Background())
	}()

	registerAndExpectStatus(t, clientConnTwo, gatewayToken, diskID, proto.StatusInvalidRequest)

	_ = clientConnTwo.Close()
	_ = clientConnOne.Close()
	_ = serverConnTwo.Close()
	_ = serverConnOne.Close()

	<-doneTwo
	<-doneOne
}

func TestLinkRuntimeRejectsWrongPhaseRequests(t *testing.T) {
	t.Parallel()

	serverConn, clientConn := net.Pipe()
	defer clientConn.Close()
	defer serverConn.Close()

	runtime := newLinkRuntime(9, serverConn, NewRegistry(), "expected-token", time.Hour, time.Hour)
	done := make(chan error, 1)
	go func() {
		done <- runtime.Serve(context.Background())
	}()

	heartbeatReq := make([]byte, proto.HeaderSize+proto.LinkHeartbeatBodySize)
	proto.EncodeHeader(proto.Header{
		ProtocolVersion: proto.ProtocolVersion,
		HeaderLen:       proto.HeaderSize,
		OpCode:          proto.OpLinkHeartbeat,
		RequestID:       1,
	}, heartbeatReq)
	copy(heartbeatReq[proto.HeaderSize:], proto.BuildLinkHeartbeatBody(1))
	if err := transport.WriteFrame(clientConn, heartbeatReq); err != nil {
		t.Fatalf("write pre-register heartbeat: %v", err)
	}

	buffer := make([]byte, transport.MaxPayloadSize)
	heartbeatResp, err := transport.ReadFrameInto(clientConn, buffer)
	if err != nil {
		t.Fatalf("read pre-register heartbeat response: %v", err)
	}
	if header := mustParseStorerHeader(t, heartbeatResp); header.StatusCode != proto.StatusInvalidRequest {
		t.Fatalf("unexpected pre-register heartbeat status: %d", header.StatusCode)
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
		GatewayToken:  gatewayToken,
		DiskID:        diskID,
		AuthVerifier:  [64]byte{1},
		DiskSizeBytes: 4096,
		MaxIOBytes:    1024,
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
	if header := mustParseStorerHeader(t, resp); header.StatusCode != want {
		t.Fatalf("unexpected register status: got=%d want=%d", header.StatusCode, want)
	}
}

func mustParseStorerHeader(t *testing.T, payload []byte) proto.Header {
	t.Helper()

	header, err := proto.ParseHeader(payload)
	if err != nil {
		t.Fatalf("parse header: %v", err)
	}
	return header
}
