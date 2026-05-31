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
	"yumedisk/server/internal/bootstrap"
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
		ClaimCodeRW:     claimCode,
		Whole:           config.StorerWholeConfig{ListenAddr: config.DefaultWholeListenAddr},
		Storer: config.StorerRemoteConfig{
			GatewayAddr:  storerListenAddr,
			GatewayToken: gatewayToken,
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
	mustGatewayHello(t, conn)

	requestID := uint64(1)
	authID := authenticateGatewayConnection(t, conn, material, &requestID)

	var openHeader proto.Header
	deadline := time.Now().Add(3 * time.Second)
	for {
		openResp := mustGatewayRoundTrip(t, conn, buildGatewayRequest(proto.OpSessionOpen, requestID, 0, proto.BuildSessionOpenRequestBody(authID)))
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
		time.Sleep(50 * time.Millisecond)
	}
	sessionID := openHeader.SessionID
	if sessionID == 0 {
		t.Fatal("expected non-zero gateway session id")
	}
	requestID++

	describeResp := mustGatewayRoundTrip(t, conn, buildGatewayRequest(proto.OpSessionDescribe, requestID, sessionID, nil))
	describeHeader := mustGatewayHeader(t, describeResp)
	if describeHeader.StatusCode != proto.StatusOK {
		t.Fatalf("describe status: %d", describeHeader.StatusCode)
	}
	diskSize, maxIOBytes, readOnly, _, err := proto.ParseSessionDescribeResponseBody(describeResp[proto.HeaderSize:])
	if err != nil {
		t.Fatalf("parse describe response: %v", err)
	}
	if diskSize != 4096 || maxIOBytes != 60*1024 || readOnly {
		t.Fatalf("unexpected describe response: size=%d maxIO=%d readOnly=%v", diskSize, maxIOBytes, readOnly)
	}
	requestID++

	writePayload := append(proto.BuildReadWriteBody(32, 4), []byte("D123")...)
	writeResp := mustGatewayRoundTrip(t, conn, buildGatewayRequest(proto.OpWriteAt, requestID, sessionID, writePayload))
	if header := mustGatewayHeader(t, writeResp); header.StatusCode != proto.StatusOK {
		t.Fatalf("write status: %d", header.StatusCode)
	}
	requestID++

	readResp := mustGatewayRoundTrip(t, conn, buildGatewayRequest(proto.OpReadAt, requestID, sessionID, proto.BuildReadBody(32, 4)))
	readHeader := mustGatewayHeader(t, readResp)
	if readHeader.StatusCode != proto.StatusOK {
		t.Fatalf("read status: %d", readHeader.StatusCode)
	}
	if !bytes.Equal(readResp[proto.HeaderSize:], proto.BuildReadResponseBody([]byte("D123"))) {
		t.Fatalf("unexpected read payload: %q", string(readResp[proto.HeaderSize:]))
	}
	requestID++

	if err := transport.WriteFrame(conn, buildGatewayNotice(
		proto.OpSessionCloseNotice,
		sessionID,
		proto.BuildSessionCloseNoticeBody(proto.SessionCloseReasonNormalClose),
	)); err != nil {
		t.Fatalf("write close notice: %v", err)
	}
	requestID++

	readAfterCloseResp := mustGatewayRoundTrip(t, conn, buildGatewayRequest(proto.OpReadAt, requestID, sessionID, proto.BuildReadBody(0, 1)))
	if header := mustGatewayHeader(t, readAfterCloseResp); header.StatusCode != proto.StatusSessionUnavailable {
		t.Fatalf("read-after-close status: %d", header.StatusCode)
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

func TestGatewayAndStorerSupportsReadOnlyExportDataPlane(t *testing.T) {
	t.Parallel()

	tempDir := t.TempDir()
	rawPath := filepath.Join(tempDir, "disk.raw")
	if err := os.WriteFile(rawPath, make([]byte, 4096), 0o644); err != nil {
		t.Fatalf("write raw file: %v", err)
	}

	rwClaimCode, err := auth.GenerateClaimCode(64)
	if err != nil {
		t.Fatalf("generate rw claim code: %v", err)
	}
	rwMaterial, err := auth.ParseClaimCode(rwClaimCode)
	if err != nil {
		t.Fatalf("parse rw claim code: %v", err)
	}
	roClaimCode, err := auth.GenerateClaimCode(64)
	if err != nil {
		t.Fatalf("generate ro claim code: %v", err)
	}
	roMaterial, err := auth.ParseClaimCode(roClaimCode)
	if err != nil {
		t.Fatalf("parse ro claim code: %v", err)
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
		ClaimCodeRW:     rwClaimCode,
		ClaimCodeRO:     roClaimCode,
		Whole:           config.StorerWholeConfig{ListenAddr: config.DefaultWholeListenAddr},
		Storer: config.StorerRemoteConfig{
			GatewayAddr:  storerListenAddr,
			GatewayToken: gatewayToken,
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

	rwConn, err := net.Dial("tcp", clientAddr)
	if err != nil {
		t.Fatalf("dial gateway rw: %v", err)
	}
	defer rwConn.Close()
	mustGatewayHello(t, rwConn)

	rwRequestID := uint64(1)
	rwAuthID := authenticateGatewayConnection(t, rwConn, rwMaterial, &rwRequestID)

	var rwOpenHeader proto.Header
	deadline := time.Now().Add(3 * time.Second)
	for {
		rwOpenResp := mustGatewayRoundTrip(t, rwConn, buildGatewayRequest(proto.OpSessionOpen, rwRequestID, 0, proto.BuildSessionOpenRequestBody(rwAuthID)))
		rwOpenHeader = mustGatewayHeader(t, rwOpenResp)
		if rwOpenHeader.StatusCode == proto.StatusOK {
			break
		}
		if time.Now().After(deadline) {
			t.Fatalf("rw session open status after registration wait: %d", rwOpenHeader.StatusCode)
		}
		if rwOpenHeader.StatusCode != proto.StatusSessionUnavailable {
			t.Fatalf("unexpected rw session open status before registration: %d", rwOpenHeader.StatusCode)
		}
		time.Sleep(50 * time.Millisecond)
	}
	rwSessionID := rwOpenHeader.SessionID
	rwRequestID++

	writePayload := append(proto.BuildReadWriteBody(0, 4), []byte("BOOT")...)
	writeResp := mustGatewayRoundTrip(t, rwConn, buildGatewayRequest(proto.OpWriteAt, rwRequestID, rwSessionID, writePayload))
	if header := mustGatewayHeader(t, writeResp); header.StatusCode != proto.StatusOK {
		t.Fatalf("rw write status: %d", header.StatusCode)
	}
	rwRequestID++

	roConn, err := net.Dial("tcp", clientAddr)
	if err != nil {
		t.Fatalf("dial gateway ro: %v", err)
	}
	defer roConn.Close()
	mustGatewayHello(t, roConn)

	roRequestID := uint64(1)
	roAuthID := authenticateGatewayConnection(t, roConn, roMaterial, &roRequestID)

	var roOpenHeader proto.Header
	deadline = time.Now().Add(3 * time.Second)
	for {
		roOpenResp := mustGatewayRoundTrip(t, roConn, buildGatewayRequest(proto.OpSessionOpen, roRequestID, 0, proto.BuildSessionOpenRequestBody(roAuthID)))
		roOpenHeader = mustGatewayHeader(t, roOpenResp)
		if roOpenHeader.StatusCode == proto.StatusOK {
			break
		}
		if time.Now().After(deadline) {
			t.Fatalf("ro session open status after registration wait: %d", roOpenHeader.StatusCode)
		}
		if roOpenHeader.StatusCode != proto.StatusSessionUnavailable {
			t.Fatalf("unexpected ro session open status before registration: %d", roOpenHeader.StatusCode)
		}
		time.Sleep(50 * time.Millisecond)
	}
	roSessionID := roOpenHeader.SessionID
	roRequestID++

	describeResp := mustGatewayRoundTrip(t, roConn, buildGatewayRequest(proto.OpSessionDescribe, roRequestID, roSessionID, nil))
	describeHeader := mustGatewayHeader(t, describeResp)
	if describeHeader.StatusCode != proto.StatusOK {
		t.Fatalf("ro describe status: %d", describeHeader.StatusCode)
	}
	diskSize, maxIOBytes, readOnly, _, err := proto.ParseSessionDescribeResponseBody(describeResp[proto.HeaderSize:])
	if err != nil {
		t.Fatalf("parse ro describe response: %v", err)
	}
	if diskSize != 4096 || maxIOBytes != 60*1024 || !readOnly {
		t.Fatalf("unexpected ro describe response: size=%d maxIO=%d readOnly=%v", diskSize, maxIOBytes, readOnly)
	}
	roRequestID++

	readResp := mustGatewayRoundTrip(t, roConn, buildGatewayRequest(proto.OpReadAt, roRequestID, roSessionID, proto.BuildReadBody(0, 4)))
	readHeader := mustGatewayHeader(t, readResp)
	if readHeader.StatusCode != proto.StatusOK {
		t.Fatalf("ro read status: %d", readHeader.StatusCode)
	}
	if !bytes.Equal(readResp[proto.HeaderSize:], proto.BuildReadResponseBody([]byte("BOOT"))) {
		t.Fatalf("unexpected ro read payload: %q", string(readResp[proto.HeaderSize:]))
	}
	roRequestID++

	writePayload = append(proto.BuildReadWriteBody(4, 4), []byte("SYNC")...)
	writeResp = mustGatewayRoundTrip(t, rwConn, buildGatewayRequest(proto.OpWriteAt, rwRequestID, rwSessionID, writePayload))
	if header := mustGatewayHeader(t, writeResp); header.StatusCode != proto.StatusOK {
		t.Fatalf("second rw write status: %d", header.StatusCode)
	}
	rwRequestID++

	dataChangedNotice := mustGatewayReadFrame(t, roConn)
	dataChangedHeader := mustGatewayHeader(t, dataChangedNotice)
	if dataChangedHeader.Flags != proto.FlagNotice {
		t.Fatalf("expected data changed notice, got flags=%d", dataChangedHeader.Flags)
	}
	if dataChangedHeader.OpCode != proto.OpSessionDataChangedNotice {
		t.Fatalf("unexpected notice op: %d", dataChangedHeader.OpCode)
	}
	if dataChangedHeader.SessionID != roSessionID {
		t.Fatalf("unexpected notice session id: %d", dataChangedHeader.SessionID)
	}
	if len(dataChangedNotice) != proto.HeaderSize {
		t.Fatalf("unexpected data changed notice body length: %d", len(dataChangedNotice)-proto.HeaderSize)
	}

	readResp = mustGatewayRoundTrip(t, roConn, buildGatewayRequest(proto.OpReadAt, roRequestID, roSessionID, proto.BuildReadBody(4, 4)))
	readHeader = mustGatewayHeader(t, readResp)
	if readHeader.StatusCode != proto.StatusOK {
		t.Fatalf("ro read after notice status: %d", readHeader.StatusCode)
	}
	if !bytes.Equal(readResp[proto.HeaderSize:], proto.BuildReadResponseBody([]byte("SYNC"))) {
		t.Fatalf("unexpected ro read-after-notice payload: %q", string(readResp[proto.HeaderSize:]))
	}
	roRequestID++

	roWritePayload := append(proto.BuildReadWriteBody(0, 4), []byte("FAIL")...)
	roWriteResp := mustGatewayRoundTrip(t, roConn, buildGatewayRequest(proto.OpWriteAt, roRequestID, roSessionID, roWritePayload))
	if header := mustGatewayHeader(t, roWriteResp); header.StatusCode != proto.StatusIOReadOnly {
		t.Fatalf("ro write status: %d", header.StatusCode)
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

func authenticateGatewayConnection(t *testing.T, conn net.Conn, material auth.Material, requestID *uint64) uint64 {
	t.Helper()

	startResp := mustGatewayRoundTrip(t, conn, buildGatewayRequest(proto.OpAuthStart, *requestID, 0, []byte(material.DiskID)))
	startHeader := mustGatewayHeader(t, startResp)
	if startHeader.StatusCode != proto.StatusOK {
		t.Fatalf("auth start status: %d", startHeader.StatusCode)
	}
	startBody, err := proto.ParseAuthStartResponseBody(startResp[proto.HeaderSize:])
	if err != nil {
		t.Fatalf("parse auth start: %v", err)
	}

	*requestID++
	proof := auth.ComputeProof(material.AuthVerifier, startBody.Salt[:])
	finishBody := proto.BuildAuthFinishRequestBody(startBody.ChallengeToken, proof)
	finishResp := mustGatewayRoundTrip(t, conn, buildGatewayRequest(proto.OpAuthFinish, *requestID, 0, finishBody))
	finishHeader := mustGatewayHeader(t, finishResp)
	if finishHeader.StatusCode != proto.StatusOK {
		t.Fatalf("auth finish status: %d", finishHeader.StatusCode)
	}
	authID, err := proto.ParseAuthFinishResponseBody(finishResp[proto.HeaderSize:])
	if err != nil {
		t.Fatalf("parse auth finish: %v", err)
	}

	*requestID++
	return authID
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
	return mustGatewayReadFrame(t, conn)
}

func mustGatewayReadFrame(t *testing.T, conn net.Conn) []byte {
	t.Helper()

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

func buildGatewayNotice(opCode uint8, sessionID uint64, body []byte) []byte {
	return proto.BuildNotice(opCode, sessionID, body)
}

func mustGatewayHello(t *testing.T, conn net.Conn) {
	t.Helper()

	response, err := bootstrap.ConnectClient(conn)
	if err != nil {
		t.Fatalf("hello bootstrap: %v", err)
	}
	if len(response.ServerCapabilities) != 0 {
		t.Fatalf("expected empty server capabilities, got %d bytes", len(response.ServerCapabilities))
	}
}
