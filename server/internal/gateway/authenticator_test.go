package gateway

import (
	"os"
	"path/filepath"
	"testing"
	"time"

	"yumedisk/server/internal/auth"
	"yumedisk/server/internal/proto"
	"yumedisk/server/internal/session"
	filestorage "yumedisk/server/internal/storage/file"
)

func TestAuthenticatorAuthStartAndFinishSuccess(t *testing.T) {
	t.Parallel()

	material := newTestMaterial(t)
	handler, err := newAuthHandler(t, material)
	if err != nil {
		t.Fatalf("new handler: %v", err)
	}

	state := handler.NewConnectionState(42)
	startResp, err := handler.HandlePayload(state, buildRequest(proto.OpAuthStart, 1, 0, []byte(material.DiskID)))
	if err != nil {
		t.Fatalf("auth start: %v", err)
	}
	startHeader := mustParseGatewayHeader(t, startResp)
	if startHeader.StatusCode != proto.StatusOK {
		t.Fatalf("unexpected auth start status: %d", startHeader.StatusCode)
	}

	startBody, err := proto.ParseAuthStartResponseBody(startResp[proto.HeaderSize:])
	if err != nil {
		t.Fatalf("parse auth start body: %v", err)
	}

	proof := auth.ComputeProof(material.AuthVerifier, startBody.Salt[:])
	finishResp, err := handler.HandlePayload(state, buildRequest(proto.OpAuthFinish, 2, 0, proto.BuildAuthFinishRequestBody(startBody.ChallengeToken, proof)))
	if err != nil {
		t.Fatalf("auth finish: %v", err)
	}
	finishHeader := mustParseGatewayHeader(t, finishResp)
	if finishHeader.StatusCode != proto.StatusOK {
		t.Fatalf("unexpected auth finish status: %d", finishHeader.StatusCode)
	}

	authID, err := proto.ParseAuthFinishResponseBody(finishResp[proto.HeaderSize:])
	if err != nil {
		t.Fatalf("parse auth finish body: %v", err)
	}
	if authID == 0 {
		t.Fatal("expected non-zero auth id")
	}

	if _, status, ok := handler.grants.Lookup(authID, state.ID); !ok || status != proto.StatusOK {
		t.Fatalf("expected issued auth grant to be available, ok=%v status=%d", ok, status)
	}
}

func TestAuthenticatorKeepsMultipleGrantedAuthIDsOnOneConnection(t *testing.T) {
	t.Parallel()

	material := newTestMaterial(t)
	handler, err := newAuthHandler(t, material)
	if err != nil {
		t.Fatalf("new handler: %v", err)
	}

	state := handler.NewConnectionState(43)
	firstAuthID := issueAuthIDForTest(t, handler, state, material)
	secondAuthID := issueAuthIDForTest(t, handler, state, material)
	if firstAuthID == secondAuthID {
		t.Fatalf("expected distinct auth ids, got %d", firstAuthID)
	}

	if _, status, ok := handler.grants.Lookup(firstAuthID, state.ID); !ok || status != proto.StatusOK {
		t.Fatalf("expected first auth grant to stay valid, ok=%v status=%d", ok, status)
	}
	if _, status, ok := handler.grants.Lookup(secondAuthID, state.ID); !ok || status != proto.StatusOK {
		t.Fatalf("expected second auth grant to stay valid, ok=%v status=%d", ok, status)
	}
}

func TestAuthenticatorFakeDiskUsesUnifiedFailure(t *testing.T) {
	t.Parallel()

	material := newTestMaterial(t)
	handler, err := newAuthHandler(t, material)
	if err != nil {
		t.Fatalf("new handler: %v", err)
	}

	state := handler.NewConnectionState(7)
	fakeDiskID := "ZZZZZZZZZZZZZZZZ"
	startResp, err := handler.HandlePayload(state, buildRequest(proto.OpAuthStart, 11, 0, []byte(fakeDiskID)))
	if err != nil {
		t.Fatalf("auth start: %v", err)
	}

	startBody, err := proto.ParseAuthStartResponseBody(startResp[proto.HeaderSize:])
	if err != nil {
		t.Fatalf("parse auth start body: %v", err)
	}

	finishResp, err := handler.HandlePayload(state, buildRequest(proto.OpAuthFinish, 12, 0, proto.BuildAuthFinishRequestBody(startBody.ChallengeToken, [proto.AuthProofSize]byte{})))
	if err != nil {
		t.Fatalf("auth finish: %v", err)
	}

	finishHeader := mustParseGatewayHeader(t, finishResp)
	if finishHeader.StatusCode != proto.StatusAuthFailed {
		t.Fatalf("unexpected fake disk auth finish status: %d", finishHeader.StatusCode)
	}
}

func TestAuthenticatorRejectsWrongPhaseRequests(t *testing.T) {
	t.Parallel()

	material := newTestMaterial(t)
	handler, err := newAuthHandler(t, material)
	if err != nil {
		t.Fatalf("new handler: %v", err)
	}

	state := handler.NewConnectionState(88)

	finishResp, err := handler.HandlePayload(state, buildRequest(proto.OpAuthFinish, 1, 0, proto.BuildAuthFinishRequestBody([]byte("token"), [proto.AuthProofSize]byte{})))
	if err != nil {
		t.Fatalf("auth finish without start: %v", err)
	}
	if header := mustParseGatewayHeader(t, finishResp); header.StatusCode != proto.StatusInvalidRequest {
		t.Fatalf("unexpected auth finish without start status: %d", header.StatusCode)
	}

	startReq := buildRequest(proto.OpAuthStart, 2, 0, []byte(material.DiskID))
	startResp, err := handler.HandlePayload(state, startReq)
	if err != nil {
		t.Fatalf("auth start: %v", err)
	}
	if header := mustParseGatewayHeader(t, startResp); header.StatusCode != proto.StatusOK {
		t.Fatalf("unexpected auth start status: %d", header.StatusCode)
	}

	restartResp, err := handler.HandlePayload(state, startReq)
	if err != nil {
		t.Fatalf("second auth start: %v", err)
	}
	if header := mustParseGatewayHeader(t, restartResp); header.StatusCode != proto.StatusInvalidRequest {
		t.Fatalf("unexpected second auth start status: %d", header.StatusCode)
	}

	startBody, err := proto.ParseAuthStartResponseBody(startResp[proto.HeaderSize:])
	if err != nil {
		t.Fatalf("parse auth start body: %v", err)
	}
	proof := auth.ComputeProof(material.AuthVerifier, startBody.Salt[:])
	successFinishResp, err := handler.HandlePayload(state, buildRequest(proto.OpAuthFinish, 3, 0, proto.BuildAuthFinishRequestBody(startBody.ChallengeToken, proof)))
	if err != nil {
		t.Fatalf("auth finish: %v", err)
	}
	if header := mustParseGatewayHeader(t, successFinishResp); header.StatusCode != proto.StatusOK {
		t.Fatalf("unexpected auth finish status: %d", header.StatusCode)
	}

	postAuthStartResp, err := handler.HandlePayload(state, startReq)
	if err != nil {
		t.Fatalf("auth start after auth finish: %v", err)
	}
	if header := mustParseGatewayHeader(t, postAuthStartResp); header.StatusCode != proto.StatusOK {
		t.Fatalf("unexpected auth start after auth finish status: %d", header.StatusCode)
	}
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

func mustParseGatewayHeader(t *testing.T, payload []byte) proto.Header {
	t.Helper()

	header, err := proto.ParseHeader(payload)
	if err != nil {
		t.Fatalf("parse header: %v", err)
	}
	return header
}

func newTestMaterial(t *testing.T) auth.Material {
	t.Helper()

	claimCode, err := auth.GenerateClaimCode(64)
	if err != nil {
		t.Fatalf("generate claim code: %v", err)
	}
	material, err := auth.ParseClaimCode(claimCode)
	if err != nil {
		t.Fatalf("parse claim code: %v", err)
	}
	return material
}

func newAuthHandler(t *testing.T, material auth.Material) (*Handler, error) {
	t.Helper()

	tempDir := t.TempDir()
	rawPath := filepath.Join(tempDir, "disk.raw")
	if err := os.WriteFile(rawPath, make([]byte, 1024), 0o644); err != nil {
		return nil, err
	}

	storage, err := filestorage.Open(rawPath, false)
	if err != nil {
		return nil, err
	}
	t.Cleanup(func() { _ = storage.Close() })

	sessions := session.NewService(session.NewManager(), storage, session.Metadata{
		DiskID:        material.DiskID,
		DiskSizeBytes: storage.Size(),
		ReadOnly:      storage.ReadOnly(),
		MaxIOBytes:    60 * 1024,
	}, 30*time.Second)
	backend := newTestGatewayBackend(material, sessions, storage.Size(), storage.ReadOnly())
	handler, err := NewHandler(backend, backend)
	if err != nil {
		return nil, err
	}
	handler.authenticator.sleep = func(time.Duration) {}
	handler.authenticator.randomDelay = func() time.Duration { return 0 }
	return handler, nil
}
