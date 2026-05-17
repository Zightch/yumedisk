package storer

import (
	"bytes"
	"context"
	"encoding/binary"
	"net"
	"os"
	"path/filepath"
	"strconv"
	"testing"
	"time"

	"yumedisk/server/internal/auth"
	"yumedisk/server/internal/config"
	"yumedisk/server/internal/proto"
	"yumedisk/server/internal/transport"
)

func TestServerMinimalClosure(t *testing.T) {
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

	cfg := config.StorerConfig{
		Role:            config.StorerRoleWhole,
		StorageFilePath: rawPath,
		ClaimCode:       claimCode,
		Whole: config.StorerWholeConfig{
			ListenAddr: reserveLocalAddr(t),
		},
		Storer: config.StorerRemoteConfig{
			GatewayAddr: config.DefaultStorerGatewayAddr,
		},
	}

	runtime, err := NewRoleRuntime(cfg)
	if err != nil {
		t.Fatalf("new runtime: %v", err)
	}
	t.Cleanup(func() { _ = runtime.Close() })

	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()

	serverDone := make(chan error, 1)
	go func() {
		serverDone <- runtime.Run(ctx)
	}()
	waitForTCP(t, cfg.Whole.ListenAddr)

	conn, err := net.Dial("tcp", cfg.Whole.ListenAddr)
	if err != nil {
		t.Fatalf("dial server: %v", err)
	}

	requestID := uint64(1)
	startResp := mustRoundTrip(t, conn, buildRequest(proto.OpAuthStart, requestID, 0, []byte(material.DiskID)))
	startHeader := mustParseHeader(t, startResp)
	if startHeader.StatusCode != proto.StatusOK {
		t.Fatalf("auth start status: %d", startHeader.StatusCode)
	}
	startBody, err := proto.ParseAuthStartResponseBody(startResp[proto.HeaderSize:])
	if err != nil {
		t.Fatalf("parse auth start response: %v", err)
	}

	requestID++
	proof := auth.ComputeProof(material.AuthVerifier, startBody.Salt[:])
	finishBody := proto.BuildAuthFinishRequestBody(startBody.ChallengeToken, proof)
	finishResp := mustRoundTrip(t, conn, buildRequest(proto.OpAuthFinish, requestID, 0, finishBody))
	finishHeader := mustParseHeader(t, finishResp)
	if finishHeader.StatusCode != proto.StatusOK {
		t.Fatalf("auth finish status: %d", finishHeader.StatusCode)
	}

	requestID++
	openResp := mustRoundTrip(t, conn, buildRequest(proto.OpSessionOpen, requestID, 0, []byte(material.DiskID)))
	openHeader := mustParseHeader(t, openResp)
	if openHeader.StatusCode != proto.StatusOK {
		t.Fatalf("session open status: %d", openHeader.StatusCode)
	}
	sessionID := openHeader.SessionID
	if sessionID == 0 {
		t.Fatal("expected non-zero session id")
	}

	requestID++
	writePayload := append(proto.BuildReadWriteBody(8, 4), []byte("YUME")...)
	writeResp := mustRoundTrip(t, conn, buildRequest(proto.OpWriteAt, requestID, sessionID, writePayload))
	writeHeader := mustParseHeader(t, writeResp)
	if writeHeader.StatusCode != proto.StatusOK {
		t.Fatalf("write status: %d", writeHeader.StatusCode)
	}

	requestID++
	readResp := mustRoundTrip(t, conn, buildRequest(proto.OpReadAt, requestID, sessionID, proto.BuildReadBody(8, 4)))
	readHeader := mustParseHeader(t, readResp)
	if readHeader.StatusCode != proto.StatusOK {
		t.Fatalf("read status: %d", readHeader.StatusCode)
	}
	if !bytes.Equal(readResp[proto.HeaderSize:], []byte("YUME")) {
		t.Fatalf("unexpected read payload: %q", string(readResp[proto.HeaderSize:]))
	}

	if err := conn.Close(); err != nil {
		t.Fatalf("close conn: %v", err)
	}
	time.Sleep(150 * time.Millisecond)

	conn2, err := net.Dial("tcp", cfg.Whole.ListenAddr)
	if err != nil {
		t.Fatalf("dial server second connection: %v", err)
	}
	defer conn2.Close()

	requestID++
	pingBody := make([]byte, 8)
	binary.BigEndian.PutUint64(pingBody, 123)
	pingResp := mustRoundTrip(t, conn2, buildRequest(proto.OpPing, requestID, sessionID, pingBody))
	pingHeader := mustParseHeader(t, pingResp)
	if pingHeader.StatusCode != proto.StatusSessionUnavailable {
		t.Fatalf("expected session unavailable after disconnect, got %d", pingHeader.StatusCode)
	}

	cancel()
	select {
	case err := <-serverDone:
		if err != nil {
			t.Fatalf("server run exited with error: %v", err)
		}
	case <-time.After(3 * time.Second):
		t.Fatal("server did not stop in time")
	}
}

func TestServerRejectsSecondSessionOpenWhileDiskIsAlreadyOpened(t *testing.T) {
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

	cfg := config.StorerConfig{
		Role:            config.StorerRoleWhole,
		StorageFilePath: rawPath,
		ClaimCode:       claimCode,
		Whole: config.StorerWholeConfig{
			ListenAddr: reserveLocalAddr(t),
		},
		Storer: config.StorerRemoteConfig{
			GatewayAddr: config.DefaultStorerGatewayAddr,
		},
	}

	runtime, err := NewRoleRuntime(cfg)
	if err != nil {
		t.Fatalf("new runtime: %v", err)
	}
	t.Cleanup(func() { _ = runtime.Close() })

	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()

	serverDone := make(chan error, 1)
	go func() {
		serverDone <- runtime.Run(ctx)
	}()
	waitForTCP(t, cfg.Whole.ListenAddr)

	connOne, err := net.Dial("tcp", cfg.Whole.ListenAddr)
	if err != nil {
		t.Fatalf("dial first client: %v", err)
	}
	defer connOne.Close()

	authenticateAndOpenSession(t, connOne, material)

	connTwo, err := net.Dial("tcp", cfg.Whole.ListenAddr)
	if err != nil {
		t.Fatalf("dial second client: %v", err)
	}
	defer connTwo.Close()

	requestID := authenticateConnection(t, connTwo, material)
	requestID++
	openResp := mustRoundTrip(t, connTwo, buildRequest(proto.OpSessionOpen, requestID, 0, []byte(material.DiskID)))
	openHeader := mustParseHeader(t, openResp)
	if openHeader.StatusCode != proto.StatusSessionBusy {
		t.Fatalf("expected session busy, got %d", openHeader.StatusCode)
	}

	cancel()
	select {
	case err := <-serverDone:
		if err != nil {
			t.Fatalf("server run exited with error: %v", err)
		}
	case <-time.After(3 * time.Second):
		t.Fatal("server did not stop in time")
	}
}

func TestStorerRuntimeServesDataPlaneWithoutClientAuth(t *testing.T) {
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

	gatewayAddr := reserveLocalAddr(t)
	cfg := config.StorerConfig{
		Role:            config.StorerRoleStorer,
		StorageFilePath: rawPath,
		ClaimCode:       claimCode,
		Whole: config.StorerWholeConfig{
			ListenAddr: config.DefaultWholeListenAddr,
		},
		Storer: config.StorerRemoteConfig{
			GatewayAddr:  gatewayAddr,
			GatewayToken: "gateway-token",
		},
	}

	listener, err := net.Listen("tcp", gatewayAddr)
	if err != nil {
		t.Fatalf("listen mock gateway: %v", err)
	}
	defer listener.Close()

	runtime, err := NewRoleRuntime(cfg)
	if err != nil {
		t.Fatalf("new runtime: %v", err)
	}
	t.Cleanup(func() { _ = runtime.Close() })

	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()

	done := make(chan error, 1)
	go func() {
		done <- runtime.Run(ctx)
	}()

	conn, err := listener.Accept()
	if err != nil {
		t.Fatalf("accept storer runtime: %v", err)
	}
	defer conn.Close()

	registerReq := proto.BuildStorerRegisterRequestBody(proto.StorerRegisterRequest{
		GatewayToken:      "gateway-token",
		DiskID:            material.DiskID,
		AuthVerifier:      material.AuthVerifier,
		DiskSizeBytes:     4096,
		MaxIOBytes:        60 * 1024,
		SessionTTLSeconds: 30,
	})
	registerPayload := make([]byte, proto.HeaderSize+len(registerReq))
	proto.EncodeHeader(proto.Header{
		ProtocolVersion: proto.ProtocolVersion,
		HeaderLen:       proto.HeaderSize,
		OpCode:          proto.OpStorerRegister,
		RequestID:       1,
	}, registerPayload)
	copy(registerPayload[proto.HeaderSize:], registerReq)
	registerResp := mustRoundTrip(t, conn, registerPayload)
	registerHeader := mustParseHeader(t, registerResp)
	if registerHeader.StatusCode != proto.StatusOK {
		t.Fatalf("register status: %d", registerHeader.StatusCode)
	}

	authResp := mustRoundTrip(t, conn, buildRequest(proto.OpAuthStart, 2, 0, []byte(material.DiskID)))
	authHeader := mustParseHeader(t, authResp)
	if authHeader.StatusCode != proto.StatusUnsupportedOp {
		t.Fatalf("expected auth op to be unsupported, got %d", authHeader.StatusCode)
	}

	openResp := mustRoundTrip(t, conn, buildRequest(proto.OpSessionOpen, 3, 0, []byte(material.DiskID)))
	openHeader := mustParseHeader(t, openResp)
	if openHeader.StatusCode != proto.StatusOK {
		t.Fatalf("session open status: %d", openHeader.StatusCode)
	}
	sessionID := openHeader.SessionID
	if sessionID == 0 {
		t.Fatal("expected non-zero storer session id")
	}

	writePayload := append(proto.BuildReadWriteBody(16, 4), []byte("DATA")...)
	writeResp := mustRoundTrip(t, conn, buildRequest(proto.OpWriteAt, 4, sessionID, writePayload))
	writeHeader := mustParseHeader(t, writeResp)
	if writeHeader.StatusCode != proto.StatusOK {
		t.Fatalf("write status: %d", writeHeader.StatusCode)
	}

	readResp := mustRoundTrip(t, conn, buildRequest(proto.OpReadAt, 5, sessionID, proto.BuildReadBody(16, 4)))
	readHeader := mustParseHeader(t, readResp)
	if readHeader.StatusCode != proto.StatusOK {
		t.Fatalf("read status: %d", readHeader.StatusCode)
	}
	if !bytes.Equal(readResp[proto.HeaderSize:], []byte("DATA")) {
		t.Fatalf("unexpected read payload: %q", string(readResp[proto.HeaderSize:]))
	}

	cancel()
	select {
	case err := <-done:
		if err != nil {
			t.Fatalf("storer runtime exited with error: %v", err)
		}
	case <-time.After(3 * time.Second):
		t.Fatal("storer runtime did not stop in time")
	}
}

func authenticateAndOpenSession(t *testing.T, conn net.Conn, material auth.Material) uint64 {
	t.Helper()

	requestID := authenticateConnection(t, conn, material)
	requestID++
	openResp := mustRoundTrip(t, conn, buildRequest(proto.OpSessionOpen, requestID, 0, []byte(material.DiskID)))
	openHeader := mustParseHeader(t, openResp)
	if openHeader.StatusCode != proto.StatusOK {
		t.Fatalf("session open status: %d", openHeader.StatusCode)
	}
	return openHeader.SessionID
}

func authenticateConnection(t *testing.T, conn net.Conn, material auth.Material) uint64 {
	t.Helper()

	requestID := uint64(1)
	startResp := mustRoundTrip(t, conn, buildRequest(proto.OpAuthStart, requestID, 0, []byte(material.DiskID)))
	startHeader := mustParseHeader(t, startResp)
	if startHeader.StatusCode != proto.StatusOK {
		t.Fatalf("auth start status: %d", startHeader.StatusCode)
	}
	startBody, err := proto.ParseAuthStartResponseBody(startResp[proto.HeaderSize:])
	if err != nil {
		t.Fatalf("parse auth start response: %v", err)
	}

	requestID++
	proof := auth.ComputeProof(material.AuthVerifier, startBody.Salt[:])
	finishBody := proto.BuildAuthFinishRequestBody(startBody.ChallengeToken, proof)
	finishResp := mustRoundTrip(t, conn, buildRequest(proto.OpAuthFinish, requestID, 0, finishBody))
	finishHeader := mustParseHeader(t, finishResp)
	if finishHeader.StatusCode != proto.StatusOK {
		t.Fatalf("auth finish status: %d", finishHeader.StatusCode)
	}
	return requestID
}

func waitForTCP(t *testing.T, addr string) {
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
	t.Fatalf("server did not start listening on %s", addr)
}

func reserveLocalAddr(t *testing.T) string {
	t.Helper()

	listener, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		t.Fatalf("reserve local addr: %v", err)
	}
	defer listener.Close()

	tcpAddr, ok := listener.Addr().(*net.TCPAddr)
	if !ok {
		t.Fatalf("unexpected listener addr type: %T", listener.Addr())
	}
	return net.JoinHostPort("127.0.0.1", strconv.Itoa(tcpAddr.Port))
}

func mustRoundTrip(t *testing.T, conn net.Conn, payload []byte) []byte {
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

func mustParseHeader(t *testing.T, payload []byte) proto.Header {
	t.Helper()

	header, err := proto.ParseHeader(payload)
	if err != nil {
		t.Fatalf("parse header: %v", err)
	}
	return header
}

func buildRequest(opCode uint8, requestID uint64, sessionID uint64, body []byte) []byte {
	payload := make([]byte, proto.HeaderSize+len(body))
	proto.EncodeHeader(proto.Header{
		ProtocolVersion: proto.ProtocolVersion,
		HeaderLen:       proto.HeaderSize,
		OpCode:          opCode,
		Flags:           0,
		StatusCode:      0,
		Reserved:        0,
		RequestID:       requestID,
		SessionID:       sessionID,
	}, payload)
	copy(payload[proto.HeaderSize:], body)
	return payload
}
