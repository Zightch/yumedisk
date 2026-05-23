package storer

import (
	"os"
	"path/filepath"
	"testing"

	"yumedisk/server/internal/auth"
	"yumedisk/server/internal/config"
)

func TestNewCoreBuildsRWExportOnly(t *testing.T) {
	t.Parallel()

	tempDir := t.TempDir()
	rawPath := filepath.Join(tempDir, "disk.raw")
	if err := os.WriteFile(rawPath, make([]byte, 4096), 0o644); err != nil {
		t.Fatalf("write raw file: %v", err)
	}

	claimCodeRW, err := auth.GenerateClaimCode(64)
	if err != nil {
		t.Fatalf("generate claim_code_rw: %v", err)
	}
	materialRW, err := auth.ParseClaimCode(claimCodeRW)
	if err != nil {
		t.Fatalf("parse claim_code_rw: %v", err)
	}

	core, err := NewCore(config.StorerConfig{
		Role:            config.StorerRoleWhole,
		StorageFilePath: rawPath,
		ClaimCodeRW:     claimCodeRW,
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

	if got := core.ExportIDs(); len(got) != 1 || got[0] != ExportIDRW {
		t.Fatalf("unexpected export ids: %+v", got)
	}
	rwExport, ok := core.Export(ExportIDRW)
	if !ok {
		t.Fatal("expected rw export")
	}
	if rwExport.DiskID() != materialRW.DiskID {
		t.Fatalf("disk id mismatch: got %q want %q", rwExport.DiskID(), materialRW.DiskID)
	}
	if rwExport.ReadOnly() {
		t.Fatal("rw export should not be read only")
	}
	if info := rwExport.GatewayRegisterInfo("gateway-token"); info.AuthVerifier != materialRW.AuthVerifier {
		t.Fatal("auth verifier mismatch")
	}
	if core.StoragePath() != rawPath {
		t.Fatalf("storage path mismatch: got %q want %q", core.StoragePath(), rawPath)
	}
	if rwExport.SessionService() == nil {
		t.Fatal("expected rw session service")
	}
	if _, ok := core.Export(ExportIDRO); ok {
		t.Fatal("unexpected ro export")
	}
}

func TestNewCoreBuildsRWAndROExportsOnSharedStorage(t *testing.T) {
	t.Parallel()

	tempDir := t.TempDir()
	rawPath := filepath.Join(tempDir, "disk.raw")
	if err := os.WriteFile(rawPath, make([]byte, 4096), 0o644); err != nil {
		t.Fatalf("write raw file: %v", err)
	}

	claimCodeRW, err := auth.GenerateClaimCode(64)
	if err != nil {
		t.Fatalf("generate claim_code_rw: %v", err)
	}
	claimCodeRO, err := auth.GenerateClaimCode(64)
	if err != nil {
		t.Fatalf("generate claim_code_ro: %v", err)
	}
	materialRW, err := auth.ParseClaimCode(claimCodeRW)
	if err != nil {
		t.Fatalf("parse claim_code_rw: %v", err)
	}
	materialRO, err := auth.ParseClaimCode(claimCodeRO)
	if err != nil {
		t.Fatalf("parse claim_code_ro: %v", err)
	}

	core, err := NewCore(config.StorerConfig{
		Role:            config.StorerRoleWhole,
		StorageFilePath: rawPath,
		ClaimCodeRW:     claimCodeRW,
		ClaimCodeRO:     claimCodeRO,
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

	if got := core.ExportIDs(); len(got) != 2 || got[0] != ExportIDRW || got[1] != ExportIDRO {
		t.Fatalf("unexpected export ids: %+v", got)
	}
	rwExport, ok := core.Export(ExportIDRW)
	if !ok {
		t.Fatal("expected rw export")
	}
	roExport, ok := core.Export(ExportIDRO)
	if !ok {
		t.Fatal("expected ro export")
	}
	if rwExport.storage != roExport.storage {
		t.Fatal("expected rw and ro exports to share storage")
	}
	if rwExport.DiskID() != materialRW.DiskID {
		t.Fatalf("unexpected rw disk id: %q", rwExport.DiskID())
	}
	if roExport.DiskID() != materialRO.DiskID {
		t.Fatalf("unexpected ro disk id: %q", roExport.DiskID())
	}
	if rwExport.ReadOnly() {
		t.Fatal("rw export should not be read only")
	}
	if !roExport.ReadOnly() {
		t.Fatal("ro export should be read only")
	}
	if rwExport.Metadata().DiskSizeBytes != roExport.Metadata().DiskSizeBytes {
		t.Fatal("expected shared disk size metadata")
	}
	if rwExport.SessionService() == nil || roExport.SessionService() == nil {
		t.Fatal("expected both session services")
	}
}
