package gateway

import (
	"testing"
	"time"

	"os"
	"path/filepath"
	"yumedisk/server/internal/auth"
	"yumedisk/server/internal/proto"
	"yumedisk/server/internal/session"
	filestorage "yumedisk/server/internal/storage/file"
)

func TestAuthenticatorAuthStartAndFinishSuccess(t *testing.T) {
	t.Parallel()

	claimCode, err := auth.GenerateClaimCode(64)
	if err != nil {
		t.Fatalf("generate claim code: %v", err)
	}
	material, err := auth.ParseClaimCode(claimCode)
	if err != nil {
		t.Fatalf("parse claim code: %v", err)
	}

	handler, err := newAuthHandler(t, material)
	if err != nil {
		t.Fatalf("new handler: %v", err)
	}

	state := handler.NewConnectionState(42)
	startReq := buildRequest(proto.OpAuthStart, 1, 0, []byte(material.DiskID))
	startResp, err := handler.HandlePayload(state, startReq)
	if err != nil {
		t.Fatalf("auth start: %v", err)
	}

	startHeader, err := proto.ParseHeader(startResp)
	if err != nil {
		t.Fatalf("parse start response header: %v", err)
	}
	if startHeader.StatusCode != proto.StatusOK {
		t.Fatalf("unexpected auth start status: %d", startHeader.StatusCode)
	}

	startBody, err := proto.ParseAuthStartResponseBody(startResp[proto.HeaderSize:])
	if err != nil {
		t.Fatalf("parse auth start body: %v", err)
	}

	proof := auth.ComputeProof(material.AuthVerifier, startBody.Salt[:])
	finishBody := proto.BuildAuthFinishRequestBody(startBody.ChallengeToken, proof)
	finishReq := buildRequest(proto.OpAuthFinish, 2, 0, finishBody)
	finishResp, err := handler.HandlePayload(state, finishReq)
	if err != nil {
		t.Fatalf("auth finish: %v", err)
	}

	finishHeader, err := proto.ParseHeader(finishResp)
	if err != nil {
		t.Fatalf("parse finish response header: %v", err)
	}
	if finishHeader.StatusCode != proto.StatusOK {
		t.Fatalf("unexpected auth finish status: %d", finishHeader.StatusCode)
	}
	if !state.isAuthenticated(material.DiskID) {
		t.Fatal("expected disk to become authenticated on connection state")
	}
}

func TestAuthenticatorFakeDiskUsesUnifiedFailure(t *testing.T) {
	t.Parallel()

	claimCode, err := auth.GenerateClaimCode(64)
	if err != nil {
		t.Fatalf("generate claim code: %v", err)
	}
	material, err := auth.ParseClaimCode(claimCode)
	if err != nil {
		t.Fatalf("parse claim code: %v", err)
	}

	handler, err := newAuthHandler(t, material)
	if err != nil {
		t.Fatalf("new handler: %v", err)
	}

	state := handler.NewConnectionState(7)
	fakeDiskID := "ZZZZZZZZZZZZZZZZ"
	startReq := buildRequest(proto.OpAuthStart, 11, 0, []byte(fakeDiskID))
	startResp, err := handler.HandlePayload(state, startReq)
	if err != nil {
		t.Fatalf("auth start: %v", err)
	}

	startBody, err := proto.ParseAuthStartResponseBody(startResp[proto.HeaderSize:])
	if err != nil {
		t.Fatalf("parse auth start body: %v", err)
	}

	var fakeProof [proto.AuthProofSize]byte
	finishBody := proto.BuildAuthFinishRequestBody(startBody.ChallengeToken, fakeProof)
	finishReq := buildRequest(proto.OpAuthFinish, 12, 0, finishBody)
	finishResp, err := handler.HandlePayload(state, finishReq)
	if err != nil {
		t.Fatalf("auth finish: %v", err)
	}

	finishHeader, err := proto.ParseHeader(finishResp)
	if err != nil {
		t.Fatalf("parse finish response header: %v", err)
	}
	if finishHeader.StatusCode != proto.StatusAuthFailed {
		t.Fatalf("unexpected fake disk auth finish status: %d", finishHeader.StatusCode)
	}
	if state.isAuthenticated(fakeDiskID) {
		t.Fatal("fake disk must not become authenticated")
	}
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

	sessions := session.NewService(session.NewManager(), storage, 30*time.Second, 60*1024)
	handler, err := NewHandler(material.DiskID, material.AuthVerifier, sessions)
	if err != nil {
		return nil, err
	}
	handler.authenticator.sleep = func(time.Duration) {}
	handler.authenticator.randomDelay = func() time.Duration { return 0 }
	return handler, nil
}
