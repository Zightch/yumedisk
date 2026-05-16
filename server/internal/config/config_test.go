package config

import (
	"bytes"
	"os"
	"path/filepath"
	"strings"
	"testing"

	"yumedisk/server/internal/auth"
)

func TestLoadOrInitCreatesConfigOnFirstRun(t *testing.T) {
	t.Parallel()

	tempDir := t.TempDir()
	configPath := filepath.Join(tempDir, "config.toml")

	var output bytes.Buffer
	cfg, initialized, err := LoadOrInit(
		configPath,
		strings.NewReader("127.0.0.1:9810\nstorage/test-disk.img\n"),
		&output,
	)
	if err != nil {
		t.Fatalf("LoadOrInit returned error: %v", err)
	}
	if !initialized {
		t.Fatal("expected first run to initialize config")
	}

	if cfg.ListenAddr != "127.0.0.1:9810" {
		t.Fatalf("unexpected listen address: %q", cfg.ListenAddr)
	}
	if cfg.StorageFilePath != "storage/test-disk.img" {
		t.Fatalf("unexpected storage file path: %q", cfg.StorageFilePath)
	}
	if _, err := auth.ParseClaimCode(cfg.ClaimCode); err != nil {
		t.Fatalf("generated claim code is invalid: %v", err)
	}

	saved, err := os.ReadFile(configPath)
	if err != nil {
		t.Fatalf("read generated config: %v", err)
	}
	if !strings.Contains(string(saved), `listen_addr = "127.0.0.1:9810"`) {
		t.Fatalf("generated config missing listen_addr: %s", string(saved))
	}
	if !strings.Contains(string(saved), `storage_file_path = "storage/test-disk.img"`) {
		t.Fatalf("generated config missing storage_file_path: %s", string(saved))
	}
	if !strings.Contains(string(saved), `claim_code = "`) {
		t.Fatalf("generated config missing claim_code: %s", string(saved))
	}

	printed := output.String()
	if !strings.Contains(printed, "generated claim_code = ") {
		t.Fatalf("expected init output to print claim code once, got: %q", printed)
	}
}

func TestLoadOrInitUsesExistingConfigWithoutReprintingClaimCode(t *testing.T) {
	t.Parallel()

	tempDir := t.TempDir()
	configPath := filepath.Join(tempDir, "config.toml")
	claimCode, err := auth.GenerateClaimCode(DefaultClaimSecretLen)
	if err != nil {
		t.Fatalf("generate claim code: %v", err)
	}

	cfg := Config{
		ListenAddr:      "127.0.0.1:9736",
		StorageFilePath: "data/existing.img",
		ClaimCode:       claimCode,
	}
	if err := Save(configPath, cfg); err != nil {
		t.Fatalf("save config: %v", err)
	}

	var output bytes.Buffer
	loaded, initialized, err := LoadOrInit(configPath, strings.NewReader(""), &output)
	if err != nil {
		t.Fatalf("LoadOrInit returned error: %v", err)
	}
	if initialized {
		t.Fatal("expected existing config to skip initialization")
	}
	if loaded != cfg {
		t.Fatalf("loaded config mismatch: got %+v want %+v", loaded, cfg)
	}
	if output.Len() != 0 {
		t.Fatalf("expected no claim code output on existing config, got: %q", output.String())
	}
}

func TestConfigPathForExecutable(t *testing.T) {
	t.Parallel()

	got := ConfigPathForExecutable(`C:\runtime\bin\storer.exe`)
	want := `C:\runtime\bin\config.toml`
	if got != want {
		t.Fatalf("config path mismatch: got %q want %q", got, want)
	}
}
