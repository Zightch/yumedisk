package storer

import (
	"context"
	"net"
	"os"
	"path/filepath"
	"testing"
	"time"

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
		ClaimCodeRW:     claimCode,
		Whole: config.StorerWholeConfig{
			ListenAddr: "127.0.0.1:9736",
		},
		Storer: config.StorerRemoteConfig{
			GatewayAddr: config.DefaultStorerGatewayAddr,
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
		ClaimCodeRW:     claimCode,
		Whole: config.StorerWholeConfig{
			ListenAddr: config.DefaultWholeListenAddr,
		},
		Storer: config.StorerRemoteConfig{
			GatewayAddr:  reserveLocalAddr(t),
			GatewayToken: "gateway-token",
		},
	})
	if err != nil {
		t.Fatalf("NewRoleRuntime returned error: %v", err)
	}
	t.Cleanup(func() { _ = runtime.Close() })

	if runtime.Role() != config.StorerRoleStorer {
		t.Fatalf("unexpected role: %q", runtime.Role())
	}
	if runtime.GatewayAddr() == "" {
		t.Fatalf("unexpected gateway addr: %q", runtime.GatewayAddr())
	}
	if runtime.ListenAddr() != "" {
		t.Fatalf("storer role should not expose listen addr, got %q", runtime.ListenAddr())
	}

	listener, err := net.Listen("tcp", runtime.GatewayAddr())
	if err != nil {
		t.Fatalf("listen mock gateway: %v", err)
	}
	defer listener.Close()

	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()

	done := make(chan error, 1)
	go func() {
		done <- runtime.Run(ctx)
	}()

	conn, err := listener.Accept()
	if err != nil {
		t.Fatalf("accept storer runtime connection: %v", err)
	}
	_ = conn.Close()

	cancel()
	select {
	case err := <-done:
		if err != nil {
			t.Fatalf("unexpected runtime error: %v", err)
		}
	case <-time.After(3 * time.Second):
		t.Fatal("storer runtime did not stop in time")
	}
}
