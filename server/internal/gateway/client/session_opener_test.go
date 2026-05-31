package client

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

func TestSessionOpenDescribeReadWriteAndClose(t *testing.T) {
	t.Parallel()

	handler, state, material, rawPath := newSessionTestHandlerWithRaw(t, false)
	authID := issueAuthIDForTest(t, handler, state, material)
	sessionID := openSessionForTest(t, handler, state, authID)

	describeResp, err := handler.HandlePayload(state, buildRequest(proto.OpSessionDescribe, 30, sessionID, nil))
	if err != nil {
		t.Fatalf("describe: %v", err)
	}
	describeHeader := mustParseGatewayHeader(t, describeResp)
	if describeHeader.StatusCode != proto.StatusOK {
		t.Fatalf("unexpected describe status: %d", describeHeader.StatusCode)
	}
	diskSize, readOnly, _, err := proto.ParseSessionDescribeResponseBody(describeResp[proto.HeaderSize:])
	if err != nil {
		t.Fatalf("parse describe body: %v", err)
	}
	if diskSize != 4096 || readOnly {
		t.Fatalf("unexpected describe body: size=%d readOnly=%v", diskSize, readOnly)
	}

	writePayload := append(proto.BuildReadWriteBody(4, 4), []byte("ABCD")...)
	writeResp, err := handler.HandlePayload(state, buildRequest(proto.OpWriteAt, 31, sessionID, writePayload))
	if err != nil {
		t.Fatalf("write: %v", err)
	}
	if header := mustParseGatewayHeader(t, writeResp); header.StatusCode != proto.StatusOK {
		t.Fatalf("unexpected write status: %d", header.StatusCode)
	}

	readResp, err := handler.HandlePayload(state, buildRequest(proto.OpReadAt, 32, sessionID, proto.BuildReadBody(4, 4)))
	if err != nil {
		t.Fatalf("read: %v", err)
	}
	readHeader := mustParseGatewayHeader(t, readResp)
	if readHeader.StatusCode != proto.StatusOK {
		t.Fatalf("unexpected read status: %d", readHeader.StatusCode)
	}
	if !bytes.Equal(readResp[proto.HeaderSize:], proto.BuildReadResponseBody([]byte("ABCD"))) {
		t.Fatalf("unexpected read payload: %q", string(readResp[proto.HeaderSize:]))
	}

	raw, err := os.ReadFile(rawPath)
	if err != nil {
		t.Fatalf("read raw file: %v", err)
	}
	if !bytes.Equal(raw[4:8], []byte("ABCD")) {
		t.Fatalf("unexpected raw backend content: %q", string(raw[4:8]))
	}

	closeResp, err := handler.HandlePayload(state, buildNotice(
		proto.OpSessionCloseNotice,
		sessionID,
		proto.BuildSessionCloseNoticeBody(proto.SessionCloseReasonNormalClose),
	))
	if err != nil {
		t.Fatalf("close notice: %v", err)
	}
	if closeResp != nil {
		t.Fatal("expected close notice to produce no response")
	}

	readAfterCloseResp, err := handler.HandlePayload(state, buildRequest(proto.OpReadAt, 34, sessionID, proto.BuildReadBody(0, 1)))
	if err != nil {
		t.Fatalf("read after close: %v", err)
	}
	if header := mustParseGatewayHeader(t, readAfterCloseResp); header.StatusCode != proto.StatusSessionUnavailable {
		t.Fatalf("unexpected read-after-close status: %d", header.StatusCode)
	}
}

func TestSessionOpenRejectsWhenSessionIsAlreadyLive(t *testing.T) {
	t.Parallel()

	handler, stateOne, material := newSessionTestHandler(t)
	authIDOne := issueAuthIDForTest(t, handler, stateOne, material)
	firstSessionID := openSessionForTest(t, handler, stateOne, authIDOne)
	if firstSessionID == 0 {
		t.Fatal("expected non-zero first session id")
	}

	stateTwo := handler.NewConnectionState(101)
	authIDTwo := issueAuthIDForTest(t, handler, stateTwo, material)

	secondOpenResp, err := handler.HandlePayload(stateTwo, buildRequest(proto.OpSessionOpen, 40, 0, proto.BuildSessionOpenRequestBody(authIDTwo)))
	if err != nil {
		t.Fatalf("open session on second client: %v", err)
	}
	secondOpenHeader := mustParseGatewayHeader(t, secondOpenResp)
	if secondOpenHeader.StatusCode != proto.StatusSessionOpenRejected {
		t.Fatalf("unexpected second open status: %d", secondOpenHeader.StatusCode)
	}
	if secondOpenHeader.SessionID != 0 {
		t.Fatalf("expected zero second session id, got %d", secondOpenHeader.SessionID)
	}
	if _, status, ok := handler.grants.Lookup(authIDTwo, stateTwo.ConnectionID()); !ok || status != proto.StatusOK {
		t.Fatalf("expected rejected auth grant to stay valid, ok=%v status=%d", ok, status)
	}

	closeResp, err := handler.HandlePayload(stateOne, buildNotice(
		proto.OpSessionCloseNotice,
		firstSessionID,
		proto.BuildSessionCloseNoticeBody(proto.SessionCloseReasonNormalClose),
	))
	if err != nil {
		t.Fatalf("close first session notice: %v", err)
	}
	if closeResp != nil {
		t.Fatal("expected close notice to produce no response")
	}

	retryOpenResp, err := handler.HandlePayload(stateTwo, buildRequest(proto.OpSessionOpen, 42, 0, proto.BuildSessionOpenRequestBody(authIDTwo)))
	if err != nil {
		t.Fatalf("retry open after rejection: %v", err)
	}
	retryOpenHeader := mustParseGatewayHeader(t, retryOpenResp)
	if retryOpenHeader.StatusCode != proto.StatusOK {
		t.Fatalf("unexpected retry open status: %d", retryOpenHeader.StatusCode)
	}
	if retryOpenHeader.SessionID == 0 {
		t.Fatal("expected non-zero retry session id")
	}
}

func TestSessionOpenReturnsUnavailableWhenRouteIsGone(t *testing.T) {
	t.Parallel()

	handler, state, material := newSessionTestHandler(t)
	authID := issueAuthIDForTest(t, handler, state, material)
	backend := handler.authenticator.RouteSource().(*testGatewayBackend)
	backend.DisconnectRoute()

	openResp, err := handler.HandlePayload(state, buildRequest(proto.OpSessionOpen, 50, 0, proto.BuildSessionOpenRequestBody(authID)))
	if err != nil {
		t.Fatalf("open session without route: %v", err)
	}
	if header := mustParseGatewayHeader(t, openResp); header.StatusCode != proto.StatusSessionUnavailable {
		t.Fatalf("unexpected open-without-route status: %d", header.StatusCode)
	}
}

func TestSessionRequestValidationAndUnavailableSemantics(t *testing.T) {
	t.Parallel()

	handler, state, material := newSessionTestHandler(t)

	openWithoutAuthResp, err := handler.HandlePayload(state, buildRequest(proto.OpSessionOpen, 60, 0, proto.BuildSessionOpenRequestBody(1)))
	if err != nil {
		t.Fatalf("open without auth grant: %v", err)
	}
	if header := mustParseGatewayHeader(t, openWithoutAuthResp); header.StatusCode != proto.StatusAuthIDInvalid {
		t.Fatalf("unexpected open without auth status: %d", header.StatusCode)
	}

	startResp, err := handler.HandlePayload(state, buildRequest(proto.OpAuthStart, 61, 0, []byte(material.DiskID)))
	if err != nil {
		t.Fatalf("auth start: %v", err)
	}
	if header := mustParseGatewayHeader(t, startResp); header.StatusCode != proto.StatusOK {
		t.Fatalf("unexpected auth start status: %d", header.StatusCode)
	}

	openWhileAuthPendingResp, err := handler.HandlePayload(state, buildRequest(proto.OpSessionOpen, 62, 0, proto.BuildSessionOpenRequestBody(1)))
	if err != nil {
		t.Fatalf("open while auth pending: %v", err)
	}
	if header := mustParseGatewayHeader(t, openWhileAuthPendingResp); header.StatusCode != proto.StatusInvalidRequest {
		t.Fatalf("unexpected open-while-auth-pending status: %d", header.StatusCode)
	}

	startBody, err := proto.ParseAuthStartResponseBody(startResp[proto.HeaderSize:])
	if err != nil {
		t.Fatalf("parse auth start body: %v", err)
	}
	proof := auth.ComputeProof(material.AuthVerifier, startBody.Salt[:])
	finishResp, err := handler.HandlePayload(state, buildRequest(proto.OpAuthFinish, 63, 0, proto.BuildAuthFinishRequestBody(startBody.ChallengeToken, proof)))
	if err != nil {
		t.Fatalf("auth finish: %v", err)
	}
	if header := mustParseGatewayHeader(t, finishResp); header.StatusCode != proto.StatusOK {
		t.Fatalf("unexpected auth finish status: %d", header.StatusCode)
	}
	authID, err := proto.ParseAuthFinishResponseBody(finishResp[proto.HeaderSize:])
	if err != nil {
		t.Fatalf("parse auth finish body: %v", err)
	}

	sessionID := openSessionForTest(t, handler, state, authID)
	wrongSessionID := sessionID + 1
	for _, tc := range []struct {
		id   uint64
		op   uint8
		body []byte
	}{
		{64, proto.OpSessionDescribe, nil},
		{65, proto.OpReadAt, proto.BuildReadBody(0, 1)},
		{66, proto.OpWriteAt, append(proto.BuildReadWriteBody(0, 1), 'X')},
	} {
		resp, err := handler.HandlePayload(state, buildRequest(tc.op, tc.id, wrongSessionID, tc.body))
		if err != nil {
			t.Fatalf("op %d with wrong session: %v", tc.op, err)
		}
		if header := mustParseGatewayHeader(t, resp); header.StatusCode != proto.StatusSessionUnavailable {
			t.Fatalf("unexpected wrong-session status for op %d: %d", tc.op, header.StatusCode)
		}
	}

	closeResp, err := handler.HandlePayload(state, buildNotice(
		proto.OpSessionCloseNotice,
		wrongSessionID,
		proto.BuildSessionCloseNoticeBody(proto.SessionCloseReasonNormalClose),
	))
	if err != nil {
		t.Fatalf("close wrong session notice: %v", err)
	}
	if closeResp != nil {
		t.Fatal("expected wrong-session close notice to be ignored without response")
	}
}

func newSessionTestHandler(t *testing.T) (*Handler, *ConnectionState, auth.Material) {
	handler, state, material, _ := newSessionTestHandlerWithRaw(t, false)
	return handler, state, material
}

func newSessionTestHandlerWithRaw(t *testing.T, readOnly bool) (*Handler, *ConnectionState, auth.Material, string) {
	t.Helper()

	material := newTestMaterial(t)
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

	sessions := session.NewService(session.NewExclusiveManager(), storage, session.Metadata{
		DiskID:        material.DiskID,
		DiskSizeBytes: storage.Size(),
		ReadOnly:      storage.ReadOnly(),
	})
	backend := newTestGatewayBackend(material, sessions, storage.Size(), storage.ReadOnly())
	handler, err := NewHandler(backend, backend)
	if err != nil {
		t.Fatalf("new handler: %v", err)
	}
	handler.authenticator.SetFailureDelayHooks(func(time.Duration) {}, func() time.Duration { return 0 })
	state := handler.NewConnectionState(100)
	return handler, state, material, rawPath
}

func issueAuthIDForTest(t *testing.T, handler *Handler, state *ConnectionState, material auth.Material) uint64 {
	t.Helper()

	startResp, err := handler.HandlePayload(state, buildRequest(proto.OpAuthStart, 10, 0, []byte(material.DiskID)))
	if err != nil {
		t.Fatalf("auth start: %v", err)
	}
	startHeader := mustParseGatewayHeader(t, startResp)
	if startHeader.StatusCode != proto.StatusOK {
		t.Fatalf("auth start status: %d", startHeader.StatusCode)
	}

	startBody, err := proto.ParseAuthStartResponseBody(startResp[proto.HeaderSize:])
	if err != nil {
		t.Fatalf("parse auth start body: %v", err)
	}
	proof := auth.ComputeProof(material.AuthVerifier, startBody.Salt[:])
	finishResp, err := handler.HandlePayload(state, buildRequest(proto.OpAuthFinish, 11, 0, proto.BuildAuthFinishRequestBody(startBody.ChallengeToken, proof)))
	if err != nil {
		t.Fatalf("auth finish: %v", err)
	}
	finishHeader := mustParseGatewayHeader(t, finishResp)
	if finishHeader.StatusCode != proto.StatusOK {
		t.Fatalf("auth finish status: %d", finishHeader.StatusCode)
	}

	authID, err := proto.ParseAuthFinishResponseBody(finishResp[proto.HeaderSize:])
	if err != nil {
		t.Fatalf("parse auth finish body: %v", err)
	}
	return authID
}

func openSessionForTest(t *testing.T, handler *Handler, state *ConnectionState, authID uint64) uint64 {
	t.Helper()

	openResp, err := handler.HandlePayload(state, buildRequest(proto.OpSessionOpen, 99, 0, proto.BuildSessionOpenRequestBody(authID)))
	if err != nil {
		t.Fatalf("open session: %v", err)
	}
	openHeader := mustParseGatewayHeader(t, openResp)
	if openHeader.StatusCode != proto.StatusOK {
		t.Fatalf("open session status: %d", openHeader.StatusCode)
	}
	return openHeader.SessionID
}
