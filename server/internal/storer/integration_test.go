package storer

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
	"yumedisk/server/internal/bootstrap"
	"yumedisk/server/internal/config"
	"yumedisk/server/internal/proto"
	"yumedisk/server/internal/transport"
)

func TestWholeRuntimeMinimalClosure(t *testing.T) {
	t.Parallel()

	rawPath, material := newRuntimeDisk(t)
	cfg := config.StorerConfig{
		Role:            config.StorerRoleWhole,
		StorageFilePath: rawPath,
		ClaimCode:       material.ClaimCode,
		Whole:           config.StorerWholeConfig{ListenAddr: reserveLocalAddr(t)},
		Storer:          config.StorerRemoteConfig{GatewayAddr: config.DefaultStorerGatewayAddr},
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
	defer conn.Close()
	mustHello(t, conn)

	requestID := uint64(1)
	authID := authenticateConnection(t, conn, material, &requestID)

	openResp := mustRoundTrip(t, conn, buildRequest(proto.OpSessionOpen, requestID, 0, proto.BuildSessionOpenRequestBody(authID)))
	openHeader := mustParseHeader(t, openResp)
	if openHeader.StatusCode != proto.StatusOK {
		t.Fatalf("session open status: %d", openHeader.StatusCode)
	}
	sessionID := openHeader.SessionID
	if sessionID == 0 {
		t.Fatal("expected non-zero session id")
	}
	requestID++

	describeResp := mustRoundTrip(t, conn, buildRequest(proto.OpSessionDescribe, requestID, sessionID, nil))
	describeHeader := mustParseHeader(t, describeResp)
	if describeHeader.StatusCode != proto.StatusOK {
		t.Fatalf("describe status: %d", describeHeader.StatusCode)
	}
	diskSize, maxIOBytes, readOnly, err := proto.ParseSessionDescribeResponseBody(describeResp[proto.HeaderSize:])
	if err != nil {
		t.Fatalf("parse describe response: %v", err)
	}
	if diskSize != 4096 || maxIOBytes != 60*1024 || readOnly {
		t.Fatalf("unexpected describe response: size=%d maxIO=%d readOnly=%v", diskSize, maxIOBytes, readOnly)
	}
	requestID++

	writePayload := append(proto.BuildReadWriteBody(8, 4), []byte("YUME")...)
	writeResp := mustRoundTrip(t, conn, buildRequest(proto.OpWriteAt, requestID, sessionID, writePayload))
	if header := mustParseHeader(t, writeResp); header.StatusCode != proto.StatusOK {
		t.Fatalf("write status: %d", header.StatusCode)
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
	requestID++

	closeResp := mustRoundTrip(t, conn, buildRequest(proto.OpClose, requestID, sessionID, nil))
	if header := mustParseHeader(t, closeResp); header.StatusCode != proto.StatusOK {
		t.Fatalf("close status: %d", header.StatusCode)
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

func TestWholeRuntimeSecondClientOpenIsRejectedButAuthIDStaysValid(t *testing.T) {
	t.Parallel()

	rawPath, material := newRuntimeDisk(t)
	cfg := config.StorerConfig{
		Role:            config.StorerRoleWhole,
		StorageFilePath: rawPath,
		ClaimCode:       material.ClaimCode,
		Whole:           config.StorerWholeConfig{ListenAddr: reserveLocalAddr(t)},
		Storer:          config.StorerRemoteConfig{GatewayAddr: config.DefaultStorerGatewayAddr},
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
	mustHello(t, connOne)

	requestIDOne := uint64(1)
	authIDOne := authenticateConnection(t, connOne, material, &requestIDOne)
	openRespOne := mustRoundTrip(t, connOne, buildRequest(proto.OpSessionOpen, requestIDOne, 0, proto.BuildSessionOpenRequestBody(authIDOne)))
	openHeaderOne := mustParseHeader(t, openRespOne)
	if openHeaderOne.StatusCode != proto.StatusOK {
		t.Fatalf("first open status: %d", openHeaderOne.StatusCode)
	}

	connTwo, err := net.Dial("tcp", cfg.Whole.ListenAddr)
	if err != nil {
		t.Fatalf("dial second client: %v", err)
	}
	defer connTwo.Close()
	mustHello(t, connTwo)

	requestIDTwo := uint64(1)
	authIDTwo := authenticateConnection(t, connTwo, material, &requestIDTwo)
	openRespTwo := mustRoundTrip(t, connTwo, buildRequest(proto.OpSessionOpen, requestIDTwo, 0, proto.BuildSessionOpenRequestBody(authIDTwo)))
	openHeaderTwo := mustParseHeader(t, openRespTwo)
	if openHeaderTwo.StatusCode != proto.StatusSessionOpenRejected {
		t.Fatalf("expected second open rejected, got %d", openHeaderTwo.StatusCode)
	}
	if openHeaderTwo.SessionID != 0 {
		t.Fatalf("expected zero second session id, got %d", openHeaderTwo.SessionID)
	}
	requestIDTwo++

	closeRespOne := mustRoundTrip(t, connOne, buildRequest(proto.OpClose, requestIDOne, openHeaderOne.SessionID, nil))
	if closeHeaderOne := mustParseHeader(t, closeRespOne); closeHeaderOne.StatusCode != proto.StatusOK {
		t.Fatalf("first close status: %d", closeHeaderOne.StatusCode)
	}
	requestIDOne++

	retryOpenRespTwo := mustRoundTrip(t, connTwo, buildRequest(proto.OpSessionOpen, requestIDTwo, 0, proto.BuildSessionOpenRequestBody(authIDTwo)))
	retryOpenHeaderTwo := mustParseHeader(t, retryOpenRespTwo)
	if retryOpenHeaderTwo.StatusCode != proto.StatusOK {
		t.Fatalf("expected retry open success, got %d", retryOpenHeaderTwo.StatusCode)
	}
	if retryOpenHeaderTwo.SessionID == 0 {
		t.Fatal("expected non-zero retry session id")
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

	rawPath, material := newRuntimeDisk(t)
	gatewayAddr := reserveLocalAddr(t)
	cfg := config.StorerConfig{
		Role:            config.StorerRoleStorer,
		StorageFilePath: rawPath,
		ClaimCode:       material.ClaimCode,
		Whole:           config.StorerWholeConfig{ListenAddr: config.DefaultWholeListenAddr},
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
		GatewayToken:  "gateway-token",
		DiskID:        material.DiskID,
		AuthVerifier:  material.AuthVerifier,
		DiskSizeBytes: 4096,
		MaxIOBytes:    60 * 1024,
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
	if header := mustParseHeader(t, registerResp); header.StatusCode != proto.StatusOK {
		t.Fatalf("register status: %d", header.StatusCode)
	}

	authResp := mustRoundTrip(t, conn, buildRequest(proto.OpAuthStart, 2, 0, []byte(material.DiskID)))
	if header := mustParseHeader(t, authResp); header.StatusCode != proto.StatusUnsupportedOp {
		t.Fatalf("expected auth op to be unsupported, got %d", header.StatusCode)
	}

	openResp := mustRoundTrip(t, conn, buildRequest(proto.OpSessionOpen, 3, 0, nil))
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
	if header := mustParseHeader(t, writeResp); header.StatusCode != proto.StatusOK {
		t.Fatalf("write status: %d", header.StatusCode)
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

func newRuntimeDisk(t *testing.T) (string, auth.Material) {
	t.Helper()

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
	return rawPath, material
}

func authenticateConnection(t *testing.T, conn net.Conn, material auth.Material, requestID *uint64) uint64 {
	t.Helper()

	startResp := mustRoundTrip(t, conn, buildRequest(proto.OpAuthStart, *requestID, 0, []byte(material.DiskID)))
	startHeader := mustParseHeader(t, startResp)
	if startHeader.StatusCode != proto.StatusOK {
		t.Fatalf("auth start status: %d", startHeader.StatusCode)
	}
	startBody, err := proto.ParseAuthStartResponseBody(startResp[proto.HeaderSize:])
	if err != nil {
		t.Fatalf("parse auth start response: %v", err)
	}

	*requestID++
	proof := auth.ComputeProof(material.AuthVerifier, startBody.Salt[:])
	finishBody := proto.BuildAuthFinishRequestBody(startBody.ChallengeToken, proof)
	finishResp := mustRoundTrip(t, conn, buildRequest(proto.OpAuthFinish, *requestID, 0, finishBody))
	finishHeader := mustParseHeader(t, finishResp)
	if finishHeader.StatusCode != proto.StatusOK {
		t.Fatalf("auth finish status: %d", finishHeader.StatusCode)
	}
	authID, err := proto.ParseAuthFinishResponseBody(finishResp[proto.HeaderSize:])
	if err != nil {
		t.Fatalf("parse auth finish response: %v", err)
	}

	*requestID++
	return authID
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
		RequestID:       requestID,
		SessionID:       sessionID,
	}, payload)
	copy(payload[proto.HeaderSize:], body)
	return payload
}

func mustHello(t *testing.T, conn net.Conn) {
	t.Helper()

	response, err := bootstrap.ConnectClient(conn)
	if err != nil {
		t.Fatalf("hello bootstrap: %v", err)
	}
	if len(response.ServerCapabilities) != 0 {
		t.Fatalf("expected empty server capabilities, got %d bytes", len(response.ServerCapabilities))
	}
}
