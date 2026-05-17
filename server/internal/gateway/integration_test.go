package gateway_test

import (
	"bytes"
	"context"
	"net"
	"os"
	"path/filepath"
	"strconv"
	"testing"
	"time"

	"yumedisk/server/internal/auth"
	"yumedisk/server/internal/config"
	"yumedisk/server/internal/gateway"
	"yumedisk/server/internal/proto"
	"yumedisk/server/internal/storer"
	"yumedisk/server/internal/transport"
)

func TestGatewayAndStorerMinimalClosure(t *testing.T) {
	t.Parallel()

	tempDir := t.TempDir()
	rawPath := filepath.Join(tempDir, "disk.raw")
	if err := os.WriteFile(rawPath, make([]byte, 4096), 0o644); err != nil {
		t.Fatalf("write raw file: %v", err)
	}

	claimCode, err := auth.GenerateClaimCode(64)
	if err != nil {
		t.Fatalf("generate claim code: %v", err)
	}
	material, err := auth.ParseClaimCode(claimCode)
	if err != nil {
		t.Fatalf("parse claim code: %v", err)
	}

	clientAddr := reserveGatewayAddr(t)
	storerListenAddr := reserveGatewayAddr(t)
	gatewayToken := "dev-gateway-token"

	gatewayRuntime, err := gateway.NewRuntime(config.GatewayConfig{
		Client: config.GatewayClientConfig{ListenAddr: clientAddr},
		Storer: config.GatewayStorerConfig{
			ListenAddr:   storerListenAddr,
			GatewayToken: gatewayToken,
		},
	})
	if err != nil {
		t.Fatalf("new gateway runtime: %v", err)
	}

	storerRuntime, err := storer.NewRoleRuntime(config.StorerConfig{
		Role:            config.StorerRoleStorer,
		StorageFilePath: rawPath,
		ClaimCode:       claimCode,
		Whole: config.StorerWholeConfig{
			ListenAddr: config.DefaultWholeListenAddr,
		},
		Storer: config.StorerRemoteConfig{
			GatewayAddr:      storerListenAddr,
			GatewayToken:     gatewayToken,
			ReconnectSeconds: 1,
		},
	})
	if err != nil {
		t.Fatalf("new storer runtime: %v", err)
	}
	t.Cleanup(func() { _ = storerRuntime.Close() })

	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()

	gatewayDone := make(chan error, 1)
	go func() {
		gatewayDone <- gatewayRuntime.Run(ctx)
	}()
	waitForGatewayAddr(t, clientAddr)
	waitForGatewayAddr(t, storerListenAddr)

	storerDone := make(chan error, 1)
	go func() {
		storerDone <- storerRuntime.Run(ctx)
	}()

	conn, err := net.Dial("tcp", clientAddr)
	if err != nil {
		t.Fatalf("dial gateway: %v", err)
	}
	defer conn.Close()

	requestID := uint64(1)
	var openHeader proto.Header
	deadline := time.Now().Add(3 * time.Second)
	for {
		startResp := mustGatewayRoundTrip(t, conn, buildGatewayRequest(proto.OpAuthStart, requestID, 0, []byte(material.DiskID)))
		startHeader := mustGatewayHeader(t, startResp)
		if startHeader.StatusCode != proto.StatusOK {
			t.Fatalf("auth start status: %d", startHeader.StatusCode)
		}
		startBody, err := proto.ParseAuthStartResponseBody(startResp[proto.HeaderSize:])
		if err != nil {
			t.Fatalf("parse auth start: %v", err)
		}

		requestID++
		proof := auth.ComputeProof(material.AuthVerifier, startBody.Salt[:])
		finishBody := proto.BuildAuthFinishRequestBody(startBody.ChallengeToken, proof)
		finishResp := mustGatewayRoundTrip(t, conn, buildGatewayRequest(proto.OpAuthFinish, requestID, 0, finishBody))
		finishHeader := mustGatewayHeader(t, finishResp)
		if finishHeader.StatusCode != proto.StatusOK {
			t.Fatalf("auth finish status: %d", finishHeader.StatusCode)
		}

		requestID++
		openResp := mustGatewayRoundTrip(t, conn, buildGatewayRequest(proto.OpSessionOpen, requestID, 0, []byte(material.DiskID)))
		openHeader = mustGatewayHeader(t, openResp)
		if openHeader.StatusCode == proto.StatusOK {
			break
		}
		if time.Now().After(deadline) {
			t.Fatalf("session open status after registration wait: %d", openHeader.StatusCode)
		}
		if openHeader.StatusCode != proto.StatusSessionUnavailable {
			t.Fatalf("unexpected session open status before registration: %d", openHeader.StatusCode)
		}
		requestID++
		time.Sleep(50 * time.Millisecond)
	}
	sessionID := openHeader.SessionID
	if sessionID == 0 {
		t.Fatal("expected non-zero gateway session id")
	}

	requestID++
	writePayload := append(proto.BuildReadWriteBody(32, 4), []byte("D123")...)
	writeResp := mustGatewayRoundTrip(t, conn, buildGatewayRequest(proto.OpWriteAt, requestID, sessionID, writePayload))
	writeHeader := mustGatewayHeader(t, writeResp)
	if writeHeader.StatusCode != proto.StatusOK {
		t.Fatalf("write status: %d", writeHeader.StatusCode)
	}

	requestID++
	readResp := mustGatewayRoundTrip(t, conn, buildGatewayRequest(proto.OpReadAt, requestID, sessionID, proto.BuildReadBody(32, 4)))
	readHeader := mustGatewayHeader(t, readResp)
	if readHeader.StatusCode != proto.StatusOK {
		t.Fatalf("read status: %d", readHeader.StatusCode)
	}
	if !bytes.Equal(readResp[proto.HeaderSize:], []byte("D123")) {
		t.Fatalf("unexpected read payload: %q", string(readResp[proto.HeaderSize:]))
	}

	requestID++
	closeResp := mustGatewayRoundTrip(t, conn, buildGatewayRequest(proto.OpClose, requestID, sessionID, nil))
	closeHeader := mustGatewayHeader(t, closeResp)
	if closeHeader.StatusCode != proto.StatusOK {
		t.Fatalf("close status: %d", closeHeader.StatusCode)
	}

	cancel()
	select {
	case err := <-gatewayDone:
		if err != nil {
			t.Fatalf("gateway runtime exited with error: %v", err)
		}
	case <-time.After(3 * time.Second):
		t.Fatal("gateway runtime did not stop in time")
	}
	select {
	case err := <-storerDone:
		if err != nil {
			t.Fatalf("storer runtime exited with error: %v", err)
		}
	case <-time.After(3 * time.Second):
		t.Fatal("storer runtime did not stop in time")
	}
}

func reserveGatewayAddr(t *testing.T) string {
	t.Helper()

	listener, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		t.Fatalf("reserve addr: %v", err)
	}
	defer listener.Close()

	tcpAddr, ok := listener.Addr().(*net.TCPAddr)
	if !ok {
		t.Fatalf("unexpected listener addr type: %T", listener.Addr())
	}
	return net.JoinHostPort("127.0.0.1", strconv.Itoa(tcpAddr.Port))
}

func waitForGatewayAddr(t *testing.T, addr string) {
	t.Helper()

	deadline := time.Now().Add(3 * time.Second)
	for time.Now().Before(deadline) {
		conn, err := net.DialTimeout("tcp", addr, 100*time.Millisecond)
		if err == nil {
			_ = conn.Close()
			return
		}
		time.Sleep(50 * time.Millisecond)
	}
	t.Fatalf("addr did not start listening: %s", addr)
}

func mustGatewayRoundTrip(t *testing.T, conn net.Conn, payload []byte) []byte {
	t.Helper()

	if err := transport.WriteFrame(conn, payload); err != nil {
		t.Fatalf("write frame: %v", err)
	}
	buffer := make([]byte, transport.MaxPayloadSize)
	resp, err := transport.ReadFrameInto(conn, buffer)
	if err != nil {
		t.Fatalf("read frame: %v", err)
	}
	out := make([]byte, len(resp))
	copy(out, resp)
	return out
}

func mustGatewayHeader(t *testing.T, payload []byte) proto.Header {
	t.Helper()

	header, err := proto.ParseHeader(payload)
	if err != nil {
		t.Fatalf("parse header: %v", err)
	}
	return header
}

func buildGatewayRequest(opCode uint8, requestID uint64, sessionID uint64, body []byte) []byte {
	payload := make([]byte, proto.HeaderSize+len(body))
	proto.EncodeHeader(proto.Header{
		ProtocolVersion: proto.ProtocolVersion,
		HeaderLen:       proto.HeaderSize,
		OpCode:          opCode,
		RequestID:       requestID,
		SessionID:       sessionID,
	}, payload)
	copy(payload[proto.HeaderSize:], body)
	return payload
}
