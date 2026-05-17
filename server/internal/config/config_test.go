package config

import (
	"bytes"
	"os"
	"path/filepath"
	"strings"
	"testing"

	"yumedisk/server/internal/auth"
)

func TestLoadOrInitStorerCreatesWholeConfigOnFirstRun(t *testing.T) {
	t.Parallel()

	tempDir := t.TempDir()
	configPath := filepath.Join(tempDir, "config.toml")

	var output bytes.Buffer
	cfg, initialized, err := LoadOrInitStorer(
		configPath,
		strings.NewReader("\nstorage/test-disk.img\n127.0.0.1:9810\n"),
		&output,
	)
	if err != nil {
		t.Fatalf("LoadOrInitStorer returned error: %v", err)
	}
	if !initialized {
		t.Fatal("expected first run to initialize config")
	}

	if cfg.Role != StorerRoleWhole {
		t.Fatalf("unexpected role: %q", cfg.Role)
	}
	if cfg.Whole.ListenAddr != "127.0.0.1:9810" {
		t.Fatalf("unexpected listen address: %q", cfg.Whole.ListenAddr)
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
	savedText := string(saved)
	if !strings.Contains(savedText, `role = "whole"`) {
		t.Fatalf("generated config missing role: %s", savedText)
	}
	if !strings.Contains(savedText, `[whole]`) {
		t.Fatalf("generated config missing [whole] section: %s", savedText)
	}
	if !strings.Contains(savedText, `listen_addr = "127.0.0.1:9810"`) {
		t.Fatalf("generated config missing whole.listen_addr: %s", savedText)
	}
	if !strings.Contains(savedText, `[storer]`) {
		t.Fatalf("generated config missing [storer] section: %s", savedText)
	}
	if !strings.Contains(savedText, `storage_file_path = "storage/test-disk.img"`) {
		t.Fatalf("generated config missing storage_file_path: %s", savedText)
	}
	if !strings.Contains(savedText, `claim_code = "`) {
		t.Fatalf("generated config missing claim_code: %s", savedText)
	}

	printed := output.String()
	if !strings.Contains(printed, "generated claim_code = ") {
		t.Fatalf("expected init output to print claim code once, got: %q", printed)
	}
}

func TestLoadOrInitStorerUsesExistingConfigWithoutReprintingClaimCode(t *testing.T) {
	t.Parallel()

	tempDir := t.TempDir()
	configPath := filepath.Join(tempDir, "config.toml")
	claimCode, err := auth.GenerateClaimCode(DefaultClaimSecretLen)
	if err != nil {
		t.Fatalf("generate claim code: %v", err)
	}

	cfg := StorerConfig{
		Role:            StorerRoleWhole,
		StorageFilePath: "data/existing.img",
		ClaimCode:       claimCode,
		Whole: StorerWholeConfig{
			ListenAddr: "127.0.0.1:9736",
		},
		Storer: StorerRemoteConfig{
			GatewayAddr:      DefaultStorerGatewayAddr,
			ReconnectSeconds: DefaultStorerReconnectSeconds,
		},
	}
	if err := SaveStorer(configPath, cfg); err != nil {
		t.Fatalf("save config: %v", err)
	}

	var output bytes.Buffer
	loaded, initialized, err := LoadOrInitStorer(configPath, strings.NewReader(""), &output)
	if err != nil {
		t.Fatalf("LoadOrInitStorer returned error: %v", err)
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

func TestLoadStorerParsesSectionedRoleConfigWithInlineComments(t *testing.T) {
	t.Parallel()

	tempDir := t.TempDir()
	configPath := filepath.Join(tempDir, "config.toml")
	claimCode, err := auth.GenerateClaimCode(DefaultClaimSecretLen)
	if err != nil {
		t.Fatalf("generate claim code: %v", err)
	}

	content := strings.Join([]string{
		`role = "storer" # whole | storer`,
		`storage_file_path = "data/disk.img"`,
		`claim_code = "` + claimCode + `"`,
		``,
		`[whole]`,
		`listen_addr = "127.0.0.1:9736"`,
		``,
		`[storer]`,
		`gateway_addr = "127.0.0.1:9836"`,
		`gateway_token = "dev-gateway-token"`,
		`reconnect_seconds = 5`,
		``,
	}, "\n")
	if err := os.WriteFile(configPath, []byte(content), 0o644); err != nil {
		t.Fatalf("write config: %v", err)
	}

	cfg, err := LoadStorer(configPath)
	if err != nil {
		t.Fatalf("LoadStorer returned error: %v", err)
	}

	if cfg.Role != StorerRoleStorer {
		t.Fatalf("unexpected role: %q", cfg.Role)
	}
	if cfg.Storer.GatewayAddr != "127.0.0.1:9836" {
		t.Fatalf("unexpected gateway addr: %q", cfg.Storer.GatewayAddr)
	}
	if cfg.Storer.GatewayToken != "dev-gateway-token" {
		t.Fatalf("unexpected gateway token: %q", cfg.Storer.GatewayToken)
	}
	if cfg.Storer.ReconnectSeconds != 5 {
		t.Fatalf("unexpected reconnect seconds: %d", cfg.Storer.ReconnectSeconds)
	}
}

func TestLoadOrInitGatewayCreatesConfigOnFirstRun(t *testing.T) {
	t.Parallel()

	tempDir := t.TempDir()
	configPath := filepath.Join(tempDir, "config.toml")

	var output bytes.Buffer
	cfg, initialized, err := LoadOrInitGateway(
		configPath,
		strings.NewReader("127.0.0.1:9737\n127.0.0.1:9837\n"),
		&output,
	)
	if err != nil {
		t.Fatalf("LoadOrInitGateway returned error: %v", err)
	}
	if !initialized {
		t.Fatal("expected first run to initialize gateway config")
	}

	if cfg.Client.ListenAddr != "127.0.0.1:9737" {
		t.Fatalf("unexpected client listen addr: %q", cfg.Client.ListenAddr)
	}
	if cfg.Storer.ListenAddr != "127.0.0.1:9837" {
		t.Fatalf("unexpected storer listen addr: %q", cfg.Storer.ListenAddr)
	}
	if strings.TrimSpace(cfg.Storer.GatewayToken) == "" {
		t.Fatal("expected generated gateway token")
	}

	saved, err := os.ReadFile(configPath)
	if err != nil {
		t.Fatalf("read generated gateway config: %v", err)
	}
	savedText := string(saved)
	if !strings.Contains(savedText, `[client]`) || !strings.Contains(savedText, `[storer]`) {
		t.Fatalf("generated gateway config missing sections: %s", savedText)
	}
	if !strings.Contains(output.String(), "generated gateway_token = ") {
		t.Fatalf("expected init output to print gateway token once, got: %q", output.String())
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
