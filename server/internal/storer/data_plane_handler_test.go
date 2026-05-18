package storer

import (
	"bytes"
	"os"
	"path/filepath"
	"testing"

	"yumedisk/server/internal/auth"
	"yumedisk/server/internal/config"
	"yumedisk/server/internal/proto"
)

func TestDataPlaneHandlerOpenReadWriteAndClose(t *testing.T) {
	t.Parallel()

	core := newTestCore(t)
	handler := newDataPlaneHandler(17, core.DiskID(), core.SessionService())

	openResp, err := handler.HandlePayload(buildRequest(proto.OpSessionOpen, 1, 0, nil))
	if err != nil {
		t.Fatalf("open session: %v", err)
	}
	openHeader := mustParseHeader(t, openResp)
	if openHeader.StatusCode != proto.StatusOK {
		t.Fatalf("unexpected open status: %d", openHeader.StatusCode)
	}

	writePayload := append(proto.BuildReadWriteBody(4, 4), []byte("YUME")...)
	writeResp, err := handler.HandlePayload(buildRequest(proto.OpWriteAt, 2, openHeader.SessionID, writePayload))
	if err != nil {
		t.Fatalf("write: %v", err)
	}
	if header := mustParseHeader(t, writeResp); header.StatusCode != proto.StatusOK {
		t.Fatalf("unexpected write status: %d", header.StatusCode)
	}

	readResp, err := handler.HandlePayload(buildRequest(proto.OpReadAt, 3, openHeader.SessionID, proto.BuildReadBody(4, 4)))
	if err != nil {
		t.Fatalf("read: %v", err)
	}
	readHeader := mustParseHeader(t, readResp)
	if readHeader.StatusCode != proto.StatusOK {
		t.Fatalf("unexpected read status: %d", readHeader.StatusCode)
	}
	if !bytes.Equal(readResp[proto.HeaderSize:], []byte("YUME")) {
		t.Fatalf("unexpected read payload: %q", string(readResp[proto.HeaderSize:]))
	}

	closeResp, err := handler.HandlePayload(buildRequest(proto.OpClose, 4, openHeader.SessionID, nil))
	if err != nil {
		t.Fatalf("close: %v", err)
	}
	if header := mustParseHeader(t, closeResp); header.StatusCode != proto.StatusOK {
		t.Fatalf("unexpected close status: %d", header.StatusCode)
	}
}

func TestDataPlaneHandlerRejectsBadOpenBodyAndAuthOps(t *testing.T) {
	t.Parallel()

	core := newTestCore(t)
	handler := newDataPlaneHandler(18, core.DiskID(), core.SessionService())

	openResp, err := handler.HandlePayload(buildRequest(proto.OpSessionOpen, 1, 0, []byte("unexpected")))
	if err != nil {
		t.Fatalf("open with body: %v", err)
	}
	if header := mustParseHeader(t, openResp); header.StatusCode != proto.StatusBadBody {
		t.Fatalf("unexpected open-with-body status: %d", header.StatusCode)
	}

	authResp, err := handler.HandlePayload(buildRequest(proto.OpAuthStart, 2, 0, []byte(core.DiskID())))
	if err != nil {
		t.Fatalf("auth start: %v", err)
	}
	if header := mustParseHeader(t, authResp); header.StatusCode != proto.StatusUnsupportedOp {
		t.Fatalf("unexpected auth-op status: %d", header.StatusCode)
	}
}

func newTestCore(t *testing.T) *Core {
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

	core, err := NewCore(testStorerConfig(rawPath, claimCode))
	if err != nil {
		t.Fatalf("new core: %v", err)
	}
	t.Cleanup(func() { _ = core.Close() })
	return core
}

func testStorerConfig(rawPath, claimCode string) config.StorerConfig {
	return config.StorerConfig{
		Role:            config.StorerRoleStorer,
		StorageFilePath: rawPath,
		ClaimCode:       claimCode,
		Whole:           config.StorerWholeConfig{ListenAddr: config.DefaultWholeListenAddr},
		Storer: config.StorerRemoteConfig{
			GatewayAddr:  config.DefaultStorerGatewayAddr,
			GatewayToken: "gateway-token",
		},
	}
}
