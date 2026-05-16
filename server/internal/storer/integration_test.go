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

	cfg := config.Config{
		ListenAddr:      reserveLocalAddr(t),
		StorageFilePath: rawPath,
		ClaimCode:       claimCode,
	}

	service, err := NewService(cfg)
	if err != nil {
		t.Fatalf("new service: %v", err)
	}
	t.Cleanup(func() { _ = service.Close() })

	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()

	serverDone := make(chan error, 1)
	go func() {
		serverDone <- service.Run(ctx)
	}()
	waitForTCP(t, cfg.ListenAddr)

	conn, err := net.Dial("tcp", cfg.ListenAddr)
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

	conn2, err := net.Dial("tcp", cfg.ListenAddr)
	if err != nil {
		t.Fatalf("dial server second connection: %v", err)
	}
	defer conn2.Close()

	requestID++
	pingBody := make([]byte, 8)
	binary.BigEndian.PutUint64(pingBody, 123)
	pingResp := mustRoundTrip(t, conn2, buildRequest(proto.OpPing, requestID, sessionID, pingBody))
	pingHeader := mustParseHeader(t, pingResp)
	if pingHeader.StatusCode != proto.StatusSessionNotFound {
		t.Fatalf("expected session not found after disconnect, got %d", pingHeader.StatusCode)
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
