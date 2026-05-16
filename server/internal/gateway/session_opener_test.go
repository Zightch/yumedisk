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
	if pingAfterCloseHeader.StatusCode != proto.StatusSessionNotFound {
		t.Fatalf("unexpected ping-after-close status: %d", pingAfterCloseHeader.StatusCode)
	}
}

func newSessionTestHandler(t *testing.T) (*Handler, *ConnectionState, auth.Material) {
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

	storage, err := filestorage.Open(rawPath, false)
	if err != nil {
		t.Fatalf("open storage: %v", err)
	}
	t.Cleanup(func() { _ = storage.Close() })

	sessions := session.NewService(session.NewManager(), storage, 30*time.Second, 60*1024)
	handler, err := NewHandler(material.DiskID, material.AuthVerifier, sessions)
	if err != nil {
		t.Fatalf("new handler: %v", err)
	}
	handler.authenticator.sleep = func(time.Duration) {}
	handler.authenticator.randomDelay = func() time.Duration { return 0 }
	state := handler.NewConnectionState(100)
	return handler, state, material
}
