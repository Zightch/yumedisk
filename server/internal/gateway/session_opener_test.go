package gateway

import (
	"bytes"
	"os"
	"path/filepath"
	"testing"
	"time"

	"yumedisk/server/internal/auth"
	"yumedisk/server/internal/proto"
	"yumedisk/server/internal/session"
	filestorage "yumedisk/server/internal/storage/file"
)

func TestSessionOpenPingAndClose(t *testing.T) {
	t.Parallel()

	handler, state, material := newSessionTestHandler(t)
	state.markAuthenticated(material.DiskID)

	openReq := buildRequest(proto.OpSessionOpen, 20, 0, []byte(material.DiskID))
	openResp, err := handler.HandlePayload(state, openReq)
	if err != nil {
		t.Fatalf("session open: %v", err)
	}
	openHeader, err := proto.ParseHeader(openResp)
	if err != nil {
		t.Fatalf("parse open response header: %v", err)
	}
	if openHeader.StatusCode != proto.StatusOK {
		t.Fatalf("unexpected open status: %d", openHeader.StatusCode)
	}
	if openHeader.SessionID == 0 {
		t.Fatal("expected non-zero session id")
	}

	pingReq := buildRequest(proto.OpPing, 21, openHeader.SessionID, proto.BuildPingResponseBody(12345))
	pingResp, err := handler.HandlePayload(state, pingReq)
	if err != nil {
		t.Fatalf("ping: %v", err)
	}
	pingHeader, err := proto.ParseHeader(pingResp)
	if err != nil {
		t.Fatalf("parse ping response header: %v", err)
	}
	if pingHeader.StatusCode != proto.StatusOK {
		t.Fatalf("unexpected ping status: %d", pingHeader.StatusCode)
	}

	closeReq := buildRequest(proto.OpClose, 22, openHeader.SessionID, nil)
	closeResp, err := handler.HandlePayload(state, closeReq)
	if err != nil {
		t.Fatalf("close: %v", err)
	}
	closeHeader, err := proto.ParseHeader(closeResp)
	if err != nil {
		t.Fatalf("parse close response header: %v", err)
	}
	if closeHeader.StatusCode != proto.StatusOK {
		t.Fatalf("unexpected close status: %d", closeHeader.StatusCode)
	}

	pingAfterCloseResp, err := handler.HandlePayload(state, pingReq)
	if err != nil {
		t.Fatalf("ping after close: %v", err)
	}
	pingAfterCloseHeader, err := proto.ParseHeader(pingAfterCloseResp)
	if err != nil {
		t.Fatalf("parse ping-after-close response header: %v", err)
	}
	if pingAfterCloseHeader.StatusCode != proto.StatusSessionUnavailable {
		t.Fatalf("unexpected ping-after-close status: %d", pingAfterCloseHeader.StatusCode)
	}
}

func TestReadAndWriteDataPlane(t *testing.T) {
	t.Parallel()

	handler, state, material, rawPath := newSessionTestHandlerWithRaw(t, false)
	state.markAuthenticated(material.DiskID)

	sessionID := openSessionForTest(t, handler, state, material.DiskID)

	writePayload := append(proto.BuildReadWriteBody(4, uint32(len([]byte("ABCD")))), []byte("ABCD")...)
	writeReq := buildRequest(proto.OpWriteAt, 30, sessionID, writePayload)
	writeResp, err := handler.HandlePayload(state, writeReq)
	if err != nil {
		t.Fatalf("write: %v", err)
	}
	writeHeader, err := proto.ParseHeader(writeResp)
	if err != nil {
		t.Fatalf("parse write response header: %v", err)
	}
	if writeHeader.StatusCode != proto.StatusOK {
		t.Fatalf("unexpected write status: %d", writeHeader.StatusCode)
	}

	readReq := buildRequest(proto.OpReadAt, 31, sessionID, proto.BuildReadBody(4, 4))
	readResp, err := handler.HandlePayload(state, readReq)
	if err != nil {
		t.Fatalf("read: %v", err)
	}
	readHeader, err := proto.ParseHeader(readResp)
	if err != nil {
		t.Fatalf("parse read response header: %v", err)
	}
	if readHeader.StatusCode != proto.StatusOK {
		t.Fatalf("unexpected read status: %d", readHeader.StatusCode)
	}
	if !bytes.Equal(readResp[proto.HeaderSize:], []byte("ABCD")) {
		t.Fatalf("unexpected read payload: %q", string(readResp[proto.HeaderSize:]))
	}

	raw, err := os.ReadFile(rawPath)
	if err != nil {
		t.Fatalf("read raw file: %v", err)
	}
	if !bytes.Equal(raw[4:8], []byte("ABCD")) {
		t.Fatalf("unexpected raw backend content: %q", string(raw[4:8]))
	}
}

func TestWriteReadOnlyAndOutOfRangeErrors(t *testing.T) {
	t.Parallel()

	readOnlyHandler, readOnlyState, material, _ := newSessionTestHandlerWithRaw(t, true)
	readOnlyState.markAuthenticated(material.DiskID)
	readOnlySessionID := openSessionForTest(t, readOnlyHandler, readOnlyState, material.DiskID)

	writePayload := append(proto.BuildReadWriteBody(0, 1), []byte("X")...)
	writeReq := buildRequest(proto.OpWriteAt, 40, readOnlySessionID, writePayload)
	writeResp, err := readOnlyHandler.HandlePayload(readOnlyState, writeReq)
	if err != nil {
		t.Fatalf("write read-only: %v", err)
	}
	writeHeader, err := proto.ParseHeader(writeResp)
	if err != nil {
		t.Fatalf("parse write read-only response: %v", err)
	}
	if writeHeader.StatusCode != proto.StatusIOReadOnly {
		t.Fatalf("unexpected read-only status: %d", writeHeader.StatusCode)
	}

	readWriteHandler, readWriteState, material2, _ := newSessionTestHandlerWithRaw(t, false)
	readWriteState.markAuthenticated(material2.DiskID)
	sessionID := openSessionForTest(t, readWriteHandler, readWriteState, material2.DiskID)

	outOfRangeReq := buildRequest(proto.OpReadAt, 41, sessionID, proto.BuildReadBody(4090, 16))
	outOfRangeResp, err := readWriteHandler.HandlePayload(readWriteState, outOfRangeReq)
	if err != nil {
		t.Fatalf("read out-of-range: %v", err)
	}
	outOfRangeHeader, err := proto.ParseHeader(outOfRangeResp)
	if err != nil {
		t.Fatalf("parse out-of-range response: %v", err)
	}
	if outOfRangeHeader.StatusCode != proto.StatusIOOutOfRange {
		t.Fatalf("unexpected out-of-range status: %d", outOfRangeHeader.StatusCode)
	}

	tooLargeReq := buildRequest(proto.OpReadAt, 42, sessionID, proto.BuildReadBody(0, 61*1024))
	tooLargeResp, err := readWriteHandler.HandlePayload(readWriteState, tooLargeReq)
	if err != nil {
		t.Fatalf("read too-large: %v", err)
	}
	tooLargeHeader, err := proto.ParseHeader(tooLargeResp)
	if err != nil {
		t.Fatalf("parse too-large response: %v", err)
	}
	if tooLargeHeader.StatusCode != proto.StatusIOLarge {
		t.Fatalf("unexpected too-large status: %d", tooLargeHeader.StatusCode)
	}
}

func TestSessionOpenRejectsSecondClientWhileDiskIsAlreadyOpened(t *testing.T) {
	t.Parallel()

	handler, stateOne, material := newSessionTestHandler(t)
	stateOne.markAuthenticated(material.DiskID)
	firstSessionID := openSessionForTest(t, handler, stateOne, material.DiskID)
	if firstSessionID == 0 {
		t.Fatal("expected non-zero first session id")
	}

	stateTwo := handler.NewConnectionState(101)
	stateTwo.markAuthenticated(material.DiskID)
	openReq := buildRequest(proto.OpSessionOpen, 120, 0, []byte(material.DiskID))
	openResp, err := handler.HandlePayload(stateTwo, openReq)
	if err != nil {
		t.Fatalf("open session on second client: %v", err)
	}
	openHeader, err := proto.ParseHeader(openResp)
	if err != nil {
		t.Fatalf("parse second open response: %v", err)
	}
	if openHeader.StatusCode != proto.StatusSessionBusy {
		t.Fatalf("unexpected second open status: %d", openHeader.StatusCode)
	}
}

func newSessionTestHandler(t *testing.T) (*Handler, *ConnectionState, auth.Material) {
	handler, state, material, _ := newSessionTestHandlerWithRaw(t, false)
	return handler, state, material
}

func newSessionTestHandlerWithRaw(t *testing.T, readOnly bool) (*Handler, *ConnectionState, auth.Material, string) {
	t.Helper()

	claimCode, err := auth.GenerateClaimCode(64)
	if err != nil {
		t.Fatalf("generate claim code: %v", err)
	}
	material, err := auth.ParseClaimCode(claimCode)
	if err != nil {
		t.Fatalf("parse claim code: %v", err)
	}

	tempDir := t.TempDir()
	rawPath := filepath.Join(tempDir, "disk.raw")
	if err := os.WriteFile(rawPath, make([]byte, 4096), 0o644); err != nil {
		t.Fatalf("write raw file: %v", err)
	}

	storage, err := filestorage.Open(rawPath, readOnly)
	if err != nil {
		t.Fatalf("open storage: %v", err)
	}
	t.Cleanup(func() { _ = storage.Close() })

	sessions := session.NewService(session.NewManager(), storage, 30*time.Second, 60*1024)
	backend := newTestGatewayBackend(material, sessions)
	handler, err := NewHandler(backend, backend)
	if err != nil {
		t.Fatalf("new handler: %v", err)
	}
	handler.authenticator.sleep = func(time.Duration) {}
	handler.authenticator.randomDelay = func() time.Duration { return 0 }
	state := handler.NewConnectionState(100)
	return handler, state, material, rawPath
}

func openSessionForTest(t *testing.T, handler *Handler, state *ConnectionState, diskID string) uint64 {
	t.Helper()

	openReq := buildRequest(proto.OpSessionOpen, 99, 0, []byte(diskID))
	openResp, err := handler.HandlePayload(state, openReq)
	if err != nil {
		t.Fatalf("open session: %v", err)
	}
	openHeader, err := proto.ParseHeader(openResp)
	if err != nil {
		t.Fatalf("parse open response: %v", err)
	}
	if openHeader.StatusCode != proto.StatusOK {
		t.Fatalf("open session status: %d", openHeader.StatusCode)
	}
	return openHeader.SessionID
}
