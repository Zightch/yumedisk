package storer

import (
	"os"
	"path/filepath"
	"testing"

	"yumedisk/server/internal/auth"
	"yumedisk/server/internal/config"
)

func TestNewCoreBuildsLocalDiskAndSessionPlane(t *testing.T) {
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

	core, err := NewCore(config.StorerConfig{
		Role:            config.StorerRoleWhole,
		StorageFilePath: rawPath,
		ClaimCode:       claimCode,
		Whole: config.StorerWholeConfig{
			ListenAddr: "127.0.0.1:9736",
		},
		Storer: config.StorerRemoteConfig{
			GatewayAddr: config.DefaultStorerGatewayAddr,
		},
	})
	if err != nil {
		t.Fatalf("NewCore returned error: %v", err)
	}
	t.Cleanup(func() { _ = core.Close() })

	if core.DiskID() != material.DiskID {
		t.Fatalf("disk id mismatch: got %q want %q", core.DiskID(), material.DiskID)
	}
	if core.AuthVerifier() != material.AuthVerifier {
		t.Fatal("auth verifier mismatch")
	}
	if core.StoragePath() != rawPath {
		t.Fatalf("storage path mismatch: got %q want %q", core.StoragePath(), rawPath)
	}
	if core.SessionService() == nil {
		t.Fatal("expected session service")
	}
}
