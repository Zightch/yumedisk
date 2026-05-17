package storer

import (
	"context"
	"os"
	"path/filepath"
	"strings"
	"testing"

	"yumedisk/server/internal/auth"
	"yumedisk/server/internal/config"
)

func TestNewRoleRuntimeBuildsWholeRuntimeOnTopOfCore(t *testing.T) {
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

	runtime, err := NewRoleRuntime(config.StorerConfig{
		Role:            config.StorerRoleWhole,
		StorageFilePath: rawPath,
		ClaimCode:       claimCode,
		Whole: config.StorerWholeConfig{
			ListenAddr: "127.0.0.1:9736",
		},
		Storer: config.StorerRemoteConfig{
			GatewayAddr:      config.DefaultStorerGatewayAddr,
			ReconnectSeconds: config.DefaultStorerReconnectSeconds,
		},
	})
	if err != nil {
		t.Fatalf("NewRoleRuntime returned error: %v", err)
	}
	t.Cleanup(func() { _ = runtime.Close() })

	if runtime.Role() != config.StorerRoleWhole {
		t.Fatalf("unexpected role: %q", runtime.Role())
	}
	if runtime.ListenAddr() != "127.0.0.1:9736" {
		t.Fatalf("unexpected listen addr: %q", runtime.ListenAddr())
	}
	if runtime.DiskID() == "" {
		t.Fatal("expected disk id")
	}
}

func TestNewRoleRuntimeBuildsStorerRuntimeOnTopOfCore(t *testing.T) {
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

	runtime, err := NewRoleRuntime(config.StorerConfig{
		Role:            config.StorerRoleStorer,
		StorageFilePath: rawPath,
		ClaimCode:       claimCode,
		Whole: config.StorerWholeConfig{
			ListenAddr: config.DefaultWholeListenAddr,
		},
		Storer: config.StorerRemoteConfig{
			GatewayAddr:      "127.0.0.1:9836",
			GatewayToken:     "gateway-token",
			ReconnectSeconds: 3,
		},
	})
	if err != nil {
		t.Fatalf("NewRoleRuntime returned error: %v", err)
	}
	t.Cleanup(func() { _ = runtime.Close() })

	if runtime.Role() != config.StorerRoleStorer {
		t.Fatalf("unexpected role: %q", runtime.Role())
	}
	if runtime.GatewayAddr() != "127.0.0.1:9836" {
		t.Fatalf("unexpected gateway addr: %q", runtime.GatewayAddr())
	}
	if runtime.ListenAddr() != "" {
		t.Fatalf("storer role should not expose listen addr, got %q", runtime.ListenAddr())
	}

	err = runtime.Run(context.Background())
	if err == nil {
		t.Fatal("expected storer runtime placeholder error")
	}
	if !strings.Contains(err.Error(), "storer runtime not implemented yet") {
		t.Fatalf("unexpected runtime error: %v", err)
	}
}
