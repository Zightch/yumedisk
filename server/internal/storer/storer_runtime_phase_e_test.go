package storer

import (
	"context"
	"fmt"
	"net"
	"strings"
	"sync"
	"testing"
	"time"

	"yumedisk/server/internal/auth"
	"yumedisk/server/internal/config"
	"yumedisk/server/internal/proto"
	"yumedisk/server/internal/transport"
)

func TestStorerRuntimeRegistersRWOnlyWhenRODisabled(t *testing.T) {
	t.Parallel()

	rawPath, rwMaterial := newRuntimeDisk(t)
	gatewayAddr := reserveLocalAddr(t)
	listener := mustListenTCP(t, gatewayAddr)
	defer listener.Close()

	runtime, err := NewRoleRuntime(config.StorerConfig{
		Role:            config.StorerRoleStorer,
		StorageFilePath: rawPath,
		ClaimCodeRW:     rwMaterial.ClaimCode,
		Whole:           config.StorerWholeConfig{ListenAddr: config.DefaultWholeListenAddr},
		Storer: config.StorerRemoteConfig{
			GatewayAddr:  gatewayAddr,
			GatewayToken: "gateway-token",
		},
	})
	if err != nil {
		t.Fatalf("new runtime: %v", err)
	}
	t.Cleanup(func() { _ = runtime.Close() })
	setStorerRuntimeLogger(runtime, func(string, ...any) {})

	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()

	done := make(chan error, 1)
	go func() {
		done <- runtime.Run(ctx)
	}()

	event := acceptRegisteredConnection(t, listener)
	defer event.conn.Close()
	assertRegisterRequestMatches(t, event.request, rwMaterial, false)

	assertNoAdditionalAccept(t, listener, 150*time.Millisecond)

	cancel()
	select {
	case err := <-done:
		if err != nil {
			t.Fatalf("runtime exited with error: %v", err)
		}
	case <-time.After(3 * time.Second):
		t.Fatal("runtime did not stop in time")
	}
}

func TestStorerRuntimeRegistersRWAndROWithIndependentMetadata(t *testing.T) {
	t.Parallel()

	rawPath, rwMaterial := newRuntimeDisk(t)
	roClaimCode, err := auth.GenerateClaimCode(64)
	if err != nil {
		t.Fatalf("generate claim_code_ro: %v", err)
	}
	roMaterial, err := auth.ParseClaimCode(roClaimCode)
	if err != nil {
		t.Fatalf("parse claim_code_ro: %v", err)
	}

	gatewayAddr := reserveLocalAddr(t)
	listener := mustListenTCP(t, gatewayAddr)
	defer listener.Close()

	runtime, err := NewRoleRuntime(config.StorerConfig{
		Role:            config.StorerRoleStorer,
		StorageFilePath: rawPath,
		ClaimCodeRW:     rwMaterial.ClaimCode,
		ClaimCodeRO:     roClaimCode,
		Whole:           config.StorerWholeConfig{ListenAddr: config.DefaultWholeListenAddr},
		Storer: config.StorerRemoteConfig{
			GatewayAddr:  gatewayAddr,
			GatewayToken: "gateway-token",
		},
	})
	if err != nil {
		t.Fatalf("new runtime: %v", err)
	}
	t.Cleanup(func() { _ = runtime.Close() })
	setStorerRuntimeLogger(runtime, func(string, ...any) {})

	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()

	done := make(chan error, 1)
	go func() {
		done <- runtime.Run(ctx)
	}()

	first := acceptRegisteredConnection(t, listener)
	second := acceptRegisteredConnection(t, listener)
	defer first.conn.Close()
	defer second.conn.Close()

	validateRegisterPair(t, []registerEvent{first, second}, rwMaterial, roMaterial)

	cancel()
	select {
	case err := <-done:
		if err != nil {
			t.Fatalf("runtime exited with error: %v", err)
		}
	case <-time.After(3 * time.Second):
		t.Fatal("runtime did not stop in time")
	}
}

func TestStorerRuntimeReconnectsOnlyDroppedLinkAndKeepsPeerAlive(t *testing.T) {
	t.Parallel()

	rawPath, rwMaterial := newRuntimeDisk(t)
	roClaimCode, err := auth.GenerateClaimCode(64)
	if err != nil {
		t.Fatalf("generate claim_code_ro: %v", err)
	}
	roMaterial, err := auth.ParseClaimCode(roClaimCode)
	if err != nil {
		t.Fatalf("parse claim_code_ro: %v", err)
	}

	gatewayAddr := reserveLocalAddr(t)
	listener := mustListenTCP(t, gatewayAddr)
	defer listener.Close()

	runtime, err := NewRoleRuntime(config.StorerConfig{
		Role:            config.StorerRoleStorer,
		StorageFilePath: rawPath,
		ClaimCodeRW:     rwMaterial.ClaimCode,
		ClaimCodeRO:     roClaimCode,
		Whole:           config.StorerWholeConfig{ListenAddr: config.DefaultWholeListenAddr},
		Storer: config.StorerRemoteConfig{
			GatewayAddr:  gatewayAddr,
			GatewayToken: "gateway-token",
		},
	})
	if err != nil {
		t.Fatalf("new runtime: %v", err)
	}
	t.Cleanup(func() { _ = runtime.Close() })
	setStorerRuntimeRetryInterval(runtime, 30*time.Millisecond)
	setStorerRuntimeLogger(runtime, func(string, ...any) {})

	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()

	done := make(chan error, 1)
	go func() {
		done <- runtime.Run(ctx)
	}()

	first := acceptRegisteredConnection(t, listener)
	second := acceptRegisteredConnection(t, listener)
	events := []registerEvent{first, second}
	validateRegisterPair(t, events, rwMaterial, roMaterial)

	var rwEvent registerEvent
	var roEvent registerEvent
	for _, event := range events {
		if event.request.ReadOnly {
			roEvent = event
		} else {
			rwEvent = event
		}
	}
	defer roEvent.conn.Close()

	_ = rwEvent.conn.Close()
	select {
	case err := <-done:
		t.Fatalf("runtime exited early after rw disconnect: %v", err)
	case <-time.After(80 * time.Millisecond):
	}

	openResp := mustRoundTrip(t, roEvent.conn, buildRequest(proto.OpSessionOpen, 1, 0, nil))
	openHeader := mustParseHeader(t, openResp)
	if openHeader.StatusCode != proto.StatusOK {
		t.Fatalf("ro open status: %d", openHeader.StatusCode)
	}

	writePayload := append(proto.BuildReadWriteBody(0, 4), []byte("FAIL")...)
	writeResp := mustRoundTrip(t, roEvent.conn, buildRequest(proto.OpWriteAt, 2, openHeader.SessionID, writePayload))
	if header := mustParseHeader(t, writeResp); header.StatusCode != proto.StatusIOReadOnly {
		t.Fatalf("ro write status: %d", header.StatusCode)
	}

	reconnected := acceptRegisteredConnection(t, listener)
	defer reconnected.conn.Close()
	assertRegisterRequestMatches(t, reconnected.request, rwMaterial, false)
	assertNoAdditionalAccept(t, listener, 150*time.Millisecond)

	cancel()
	select {
	case err := <-done:
		if err != nil {
			t.Fatalf("runtime exited with error: %v", err)
		}
	case <-time.After(3 * time.Second):
		t.Fatal("runtime did not stop in time")
	}
}

func TestStorerRuntimeReconnectLogsStayQuietAcrossRepeatedFailures(t *testing.T) {
	t.Parallel()

	rawPath, rwMaterial := newRuntimeDisk(t)
	gatewayAddr := reserveLocalAddr(t)

	runtime, err := NewRoleRuntime(config.StorerConfig{
		Role:            config.StorerRoleStorer,
		StorageFilePath: rawPath,
		ClaimCodeRW:     rwMaterial.ClaimCode,
		Whole:           config.StorerWholeConfig{ListenAddr: config.DefaultWholeListenAddr},
		Storer: config.StorerRemoteConfig{
			GatewayAddr:  gatewayAddr,
			GatewayToken: "gateway-token",
		},
	})
	if err != nil {
		t.Fatalf("new runtime: %v", err)
	}
	t.Cleanup(func() { _ = runtime.Close() })
	setStorerRuntimeRetryInterval(runtime, 20*time.Millisecond)

	var (
		logMu sync.Mutex
		logs  []string
	)
	setStorerRuntimeLogger(runtime, func(format string, args ...any) {
		logMu.Lock()
		defer logMu.Unlock()
		logs = append(logs, formatMessage(format, args...))
	})

	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()

	done := make(chan error, 1)
	go func() {
		done <- runtime.Run(ctx)
	}()

	time.Sleep(90 * time.Millisecond)
	select {
	case err := <-done:
		t.Fatalf("runtime exited early during retry loop: %v", err)
	default:
	}

	listener := mustListenTCP(t, gatewayAddr)
	defer listener.Close()

	event := acceptRegisteredConnection(t, listener)
	defer event.conn.Close()
	assertRegisterRequestMatches(t, event.request, rwMaterial, false)

	time.Sleep(40 * time.Millisecond)
	cancel()
	select {
	case err := <-done:
		if err != nil {
			t.Fatalf("runtime exited with error: %v", err)
		}
	case <-time.After(3 * time.Second):
		t.Fatal("runtime did not stop in time")
	}

	logMu.Lock()
	defer logMu.Unlock()
	if countMessages(logs, "rw重连中...") != 1 {
		t.Fatalf("unexpected reconnect-start logs: %+v", logs)
	}
	if countMessages(logs, "rw重连成功") != 1 {
		t.Fatalf("unexpected reconnect-success logs: %+v", logs)
	}
}

type registerEvent struct {
	conn    net.Conn
	request proto.StorerRegisterRequest
}

func setStorerRuntimeRetryInterval(runtime *RoleRuntime, retryInterval time.Duration) {
	storerRuntime, ok := runtime.runtime.(*StorerRuntime)
	if !ok {
		return
	}
	for _, link := range storerRuntime.links {
		link.retryInterval = retryInterval
	}
}

func setStorerRuntimeLogger(runtime *RoleRuntime, logf func(format string, args ...any)) {
	storerRuntime, ok := runtime.runtime.(*StorerRuntime)
	if !ok {
		return
	}
	for _, link := range storerRuntime.links {
		link.logf = logf
	}
}

func mustListenTCP(t *testing.T, addr string) *net.TCPListener {
	t.Helper()

	listener, err := net.Listen("tcp", addr)
	if err != nil {
		t.Fatalf("listen %s: %v", addr, err)
	}
	tcpListener, ok := listener.(*net.TCPListener)
	if !ok {
		listener.Close()
		t.Fatalf("unexpected listener type: %T", listener)
	}
	return tcpListener
}

func acceptRegisteredConnection(t *testing.T, listener net.Listener) registerEvent {
	t.Helper()

	conn, err := listener.Accept()
	if err != nil {
		t.Fatalf("accept link connection: %v", err)
	}

	buffer := make([]byte, transport.MaxPayloadSize)
	payload, err := transport.ReadFrameInto(conn, buffer)
	if err != nil {
		conn.Close()
		t.Fatalf("read register request: %v", err)
	}
	header, err := proto.ParseHeader(payload)
	if err != nil {
		conn.Close()
		t.Fatalf("parse register header: %v", err)
	}
	if header.OpCode != proto.OpStorerRegister {
		conn.Close()
		t.Fatalf("unexpected register opcode: %d", header.OpCode)
	}
	request, err := proto.ParseStorerRegisterRequestBody(payload[proto.HeaderSize:])
	if err != nil {
		conn.Close()
		t.Fatalf("parse register body: %v", err)
	}
	if err := transport.WriteFrame(conn, proto.BuildSuccessResponse(header, nil)); err != nil {
		conn.Close()
		t.Fatalf("write register response: %v", err)
	}
	return registerEvent{
		conn:    conn,
		request: request,
	}
}

func assertRegisterRequestMatches(t *testing.T, request proto.StorerRegisterRequest, material auth.Material, readOnly bool) {
	t.Helper()

	if request.DiskID != material.DiskID {
		t.Fatalf("unexpected disk id: got=%q want=%q", request.DiskID, material.DiskID)
	}
	if request.AuthVerifier != material.AuthVerifier {
		t.Fatal("unexpected auth verifier")
	}
	if request.ReadOnly != readOnly {
		t.Fatalf("unexpected read_only: got=%v want=%v", request.ReadOnly, readOnly)
	}
	if request.DiskSizeBytes != 4096 {
		t.Fatalf("unexpected disk size: %d", request.DiskSizeBytes)
	}
	if request.MaxIOBytes != 60*1024 {
		t.Fatalf("unexpected max io bytes: %d", request.MaxIOBytes)
	}
}

func validateRegisterPair(t *testing.T, events []registerEvent, rwMaterial auth.Material, roMaterial auth.Material) {
	t.Helper()

	if len(events) != 2 {
		t.Fatalf("unexpected event count: %d", len(events))
	}

	var sawRW bool
	var sawRO bool
	for _, event := range events {
		switch {
		case event.request.DiskID == rwMaterial.DiskID:
			assertRegisterRequestMatches(t, event.request, rwMaterial, false)
			sawRW = true
		case event.request.DiskID == roMaterial.DiskID:
			assertRegisterRequestMatches(t, event.request, roMaterial, true)
			sawRO = true
		default:
			t.Fatalf("unexpected disk id: %q", event.request.DiskID)
		}
	}
	if !sawRW || !sawRO {
		t.Fatalf("missing expected rw/ro registrations: rw=%v ro=%v", sawRW, sawRO)
	}
}

func assertNoAdditionalAccept(t *testing.T, listener *net.TCPListener, timeout time.Duration) {
	t.Helper()

	if err := listener.SetDeadline(time.Now().Add(timeout)); err != nil {
		t.Fatalf("set deadline: %v", err)
	}
	defer listener.SetDeadline(time.Time{})

	conn, err := listener.Accept()
	if err == nil {
		conn.Close()
		t.Fatal("accepted unexpected additional connection")
	}
	netErr, ok := err.(net.Error)
	if !ok || !netErr.Timeout() {
		t.Fatalf("unexpected accept error: %v", err)
	}
}

func countMessages(logs []string, needle string) int {
	count := 0
	for _, line := range logs {
		if strings.Contains(line, needle) {
			count++
		}
	}
	return count
}

func formatMessage(format string, args ...any) string {
	if len(args) == 0 {
		return format
	}
	return fmt.Sprintf(format, args...)
}
