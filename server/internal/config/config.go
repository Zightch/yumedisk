package config

import (
	"bufio"
	"crypto/rand"
	"errors"
	"fmt"
	"io"
	"io/fs"
	"os"
	"path/filepath"
	"strconv"
	"strings"

	"yumedisk/server/internal/auth"
)

const (
	DefaultWholeListenAddr         = "127.0.0.1:9736"
	DefaultGatewayClientListenAddr = "127.0.0.1:9736"
	DefaultGatewayStorerListenAddr = "127.0.0.1:9836"
	DefaultStorerGatewayAddr       = "127.0.0.1:9836"
	DefaultStorerReconnectSeconds  = 3
	DefaultClaimSecretLen          = 64
	DefaultGatewayTokenLen         = 64
	DefaultConfigFileName          = "config.toml"
)

const randomSecretAlphabet = "0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ"

type StorerRole string

const (
	StorerRoleWhole  StorerRole = "whole"
	StorerRoleStorer StorerRole = "storer"
)

type StorerWholeConfig struct {
	ListenAddr string
}

type StorerRemoteConfig struct {
	GatewayAddr      string
	GatewayToken     string
	ReconnectSeconds uint32
}

type StorerConfig struct {
	Role            StorerRole
	StorageFilePath string
	ClaimCode       string
	Whole           StorerWholeConfig
	Storer          StorerRemoteConfig
}

type GatewayClientConfig struct {
	ListenAddr string
}

type GatewayStorerConfig struct {
	ListenAddr   string
	GatewayToken string
}

type GatewayConfig struct {
	Client GatewayClientConfig
	Storer GatewayStorerConfig
}

func ExecutableConfigPath() (string, error) {
	exePath, err := os.Executable()
	if err != nil {
		return "", fmt.Errorf("resolve executable path: %w", err)
	}
	return ConfigPathForExecutable(exePath), nil
}

func ConfigPathForExecutable(exePath string) string {
	return filepath.Join(filepath.Dir(exePath), DefaultConfigFileName)
}

func LoadOrInitStorer(path string, stdin io.Reader, stdout io.Writer) (StorerConfig, bool, error) {
	cfg, err := LoadStorer(path)
	if err == nil {
		return cfg, false, nil
	}
	if !errors.Is(err, fs.ErrNotExist) {
		return StorerConfig{}, false, err
	}

	cfg, err = promptAndCreateStorer(path, stdin, stdout)
	if err != nil {
		return StorerConfig{}, false, err
	}
	return cfg, true, nil
}

func LoadOrInitGateway(path string, stdin io.Reader, stdout io.Writer) (GatewayConfig, bool, error) {
	cfg, err := LoadGateway(path)
	if err == nil {
		return cfg, false, nil
	}
	if !errors.Is(err, fs.ErrNotExist) {
		return GatewayConfig{}, false, err
	}

	cfg, err = promptAndCreateGateway(path, stdin, stdout)
	if err != nil {
		return GatewayConfig{}, false, err
	}
	return cfg, true, nil
}

func LoadStorer(path string) (StorerConfig, error) {
	values, err := loadKeyValues(path)
	if err != nil {
		return StorerConfig{}, err
	}

	cfg := StorerConfig{
		Role:            StorerRole(getString(values, "role", "")),
		StorageFilePath: getString(values, "storage_file_path", ""),
		ClaimCode:       getString(values, "claim_code", ""),
		Whole: StorerWholeConfig{
			ListenAddr: getString(values, "whole.listen_addr", DefaultWholeListenAddr),
		},
		Storer: StorerRemoteConfig{
			GatewayAddr:      getString(values, "storer.gateway_addr", DefaultStorerGatewayAddr),
			GatewayToken:     getString(values, "storer.gateway_token", ""),
			ReconnectSeconds: getUint32(values, "storer.reconnect_seconds", DefaultStorerReconnectSeconds),
		},
	}
	if err := cfg.Validate(); err != nil {
		return StorerConfig{}, err
	}
	return cfg, nil
}

func LoadGateway(path string) (GatewayConfig, error) {
	values, err := loadKeyValues(path)
	if err != nil {
		return GatewayConfig{}, err
	}

	cfg := GatewayConfig{
		Client: GatewayClientConfig{
			ListenAddr: getString(values, "client.listen_addr", DefaultGatewayClientListenAddr),
		},
		Storer: GatewayStorerConfig{
			ListenAddr:   getString(values, "storer.listen_addr", DefaultGatewayStorerListenAddr),
			GatewayToken: getString(values, "storer.gateway_token", ""),
		},
	}
	if err := cfg.Validate(); err != nil {
		return GatewayConfig{}, err
	}
	return cfg, nil
}

func SaveStorer(path string, cfg StorerConfig) error {
	if err := cfg.Validate(); err != nil {
		return err
	}
	if err := os.MkdirAll(filepath.Dir(path), 0o755); err != nil {
		return fmt.Errorf("create config directory: %w", err)
	}

	content := fmt.Sprintf(
		"role = %q\nstorage_file_path = %q\nclaim_code = %q\n\n[whole]\nlisten_addr = %q\n\n[storer]\ngateway_addr = %q\ngateway_token = %q\nreconnect_seconds = %d\n",
		string(cfg.Role),
		cfg.StorageFilePath,
		cfg.ClaimCode,
		cfg.Whole.ListenAddr,
		cfg.Storer.GatewayAddr,
		cfg.Storer.GatewayToken,
		cfg.Storer.ReconnectSeconds,
	)
	if err := os.WriteFile(path, []byte(content), 0o644); err != nil {
		return fmt.Errorf("write config file: %w", err)
	}
	return nil
}

func SaveGateway(path string, cfg GatewayConfig) error {
	if err := cfg.Validate(); err != nil {
		return err
	}
	if err := os.MkdirAll(filepath.Dir(path), 0o755); err != nil {
		return fmt.Errorf("create config directory: %w", err)
	}

	content := fmt.Sprintf(
		"[client]\nlisten_addr = %q\n\n[storer]\nlisten_addr = %q\ngateway_token = %q\n",
		cfg.Client.ListenAddr,
		cfg.Storer.ListenAddr,
		cfg.Storer.GatewayToken,
	)
	if err := os.WriteFile(path, []byte(content), 0o644); err != nil {
		return fmt.Errorf("write config file: %w", err)
	}
	return nil
}

func (c StorerConfig) Validate() error {
	switch c.Role {
	case StorerRoleWhole, StorerRoleStorer:
	default:
		return fmt.Errorf("invalid role: %q", c.Role)
	}

	if strings.TrimSpace(c.StorageFilePath) == "" {
		return errors.New("storage_file_path must not be empty")
	}
	if _, err := auth.ParseClaimCode(c.ClaimCode); err != nil {
		return fmt.Errorf("invalid claim_code: %w", err)
	}

	switch c.Role {
	case StorerRoleWhole:
		if strings.TrimSpace(c.Whole.ListenAddr) == "" {
			return errors.New("whole.listen_addr must not be empty")
		}
	case StorerRoleStorer:
		if strings.TrimSpace(c.Storer.GatewayAddr) == "" {
			return errors.New("storer.gateway_addr must not be empty")
		}
		if strings.TrimSpace(c.Storer.GatewayToken) == "" {
			return errors.New("storer.gateway_token must not be empty")
		}
		if c.Storer.ReconnectSeconds == 0 {
			return errors.New("storer.reconnect_seconds must be > 0")
		}
	}
	return nil
}

func (c GatewayConfig) Validate() error {
	if strings.TrimSpace(c.Client.ListenAddr) == "" {
		return errors.New("client.listen_addr must not be empty")
	}
	if strings.TrimSpace(c.Storer.ListenAddr) == "" {
		return errors.New("storer.listen_addr must not be empty")
	}
	if strings.TrimSpace(c.Storer.GatewayToken) == "" {
		return errors.New("storer.gateway_token must not be empty")
	}
	return nil
}

func promptAndCreateStorer(path string, stdin io.Reader, stdout io.Writer) (StorerConfig, error) {
	reader := bufio.NewReader(stdin)

	roleText, err := promptLine(reader, stdout, "role", string(StorerRoleWhole))
	if err != nil {
		return StorerConfig{}, err
	}
	role := StorerRole(roleText)
	if role != StorerRoleWhole && role != StorerRoleStorer {
		return StorerConfig{}, fmt.Errorf("invalid role: %q", role)
	}

	storageFilePath, err := promptLine(reader, stdout, "storage_file_path", "data/disk.img")
	if err != nil {
		return StorerConfig{}, err
	}

	claimCode, err := auth.GenerateClaimCode(DefaultClaimSecretLen)
	if err != nil {
		return StorerConfig{}, fmt.Errorf("generate claim code: %w", err)
	}

	cfg := StorerConfig{
		Role:            role,
		StorageFilePath: storageFilePath,
		ClaimCode:       claimCode,
		Whole: StorerWholeConfig{
			ListenAddr: DefaultWholeListenAddr,
		},
		Storer: StorerRemoteConfig{
			GatewayAddr:      DefaultStorerGatewayAddr,
			ReconnectSeconds: DefaultStorerReconnectSeconds,
		},
	}

	switch role {
	case StorerRoleWhole:
		cfg.Whole.ListenAddr, err = promptLine(reader, stdout, "whole.listen_addr", DefaultWholeListenAddr)
		if err != nil {
			return StorerConfig{}, err
		}
	case StorerRoleStorer:
		cfg.Storer.GatewayAddr, err = promptLine(reader, stdout, "storer.gateway_addr", DefaultStorerGatewayAddr)
		if err != nil {
			return StorerConfig{}, err
		}
		cfg.Storer.GatewayToken, err = promptRequiredLine(reader, stdout, "storer.gateway_token")
		if err != nil {
			return StorerConfig{}, err
		}
	}

	if err := SaveStorer(path, cfg); err != nil {
		return StorerConfig{}, err
	}

	fmt.Fprintf(stdout, "generated claim_code = %s\n", cfg.ClaimCode)
	return cfg, nil
}

func promptAndCreateGateway(path string, stdin io.Reader, stdout io.Writer) (GatewayConfig, error) {
	reader := bufio.NewReader(stdin)

	clientListenAddr, err := promptLine(reader, stdout, "client.listen_addr", DefaultGatewayClientListenAddr)
	if err != nil {
		return GatewayConfig{}, err
	}
	storerListenAddr, err := promptLine(reader, stdout, "storer.listen_addr", DefaultGatewayStorerListenAddr)
	if err != nil {
		return GatewayConfig{}, err
	}
	gatewayToken, err := generateRandomSecret(DefaultGatewayTokenLen)
	if err != nil {
		return GatewayConfig{}, fmt.Errorf("generate gateway token: %w", err)
	}

	cfg := GatewayConfig{
		Client: GatewayClientConfig{
			ListenAddr: clientListenAddr,
		},
		Storer: GatewayStorerConfig{
			ListenAddr:   storerListenAddr,
			GatewayToken: gatewayToken,
		},
	}
	if err := SaveGateway(path, cfg); err != nil {
		return GatewayConfig{}, err
	}

	fmt.Fprintf(stdout, "generated gateway_token = %s\n", cfg.Storer.GatewayToken)
	return cfg, nil
}

func promptLine(reader *bufio.Reader, stdout io.Writer, key, fallback string) (string, error) {
	if _, err := fmt.Fprintf(stdout, "%s [%s]: ", key, fallback); err != nil {
		return "", err
	}
	line, err := reader.ReadString('\n')
	if err != nil && !errors.Is(err, io.EOF) {
		return "", err
	}
	line = strings.TrimSpace(line)
	if line == "" {
		return fallback, nil
	}
	return line, nil
}

func promptRequiredLine(reader *bufio.Reader, stdout io.Writer, key string) (string, error) {
	if _, err := fmt.Fprintf(stdout, "%s: ", key); err != nil {
		return "", err
	}
	line, err := reader.ReadString('\n')
	if err != nil && !errors.Is(err, io.EOF) {
		return "", err
	}
	line = strings.TrimSpace(line)
	if line == "" {
		return "", fmt.Errorf("%s must not be empty", key)
	}
	return line, nil
}

func loadKeyValues(path string) (map[string]string, error) {
	data, err := os.ReadFile(path)
	if err != nil {
		return nil, err
	}

	values, err := parseTOML(string(data))
	if err != nil {
		return nil, fmt.Errorf("parse %s: %w", path, err)
	}
	return values, nil
}

func parseTOML(content string) (map[string]string, error) {
	values := make(map[string]string)
	scanner := bufio.NewScanner(strings.NewReader(content))
	lineNo := 0
	section := ""

	for scanner.Scan() {
		lineNo++
		line := strings.TrimSpace(stripInlineComment(scanner.Text()))
		if line == "" {
			continue
		}

		if strings.HasPrefix(line, "[") {
			if !strings.HasSuffix(line, "]") {
				return nil, fmt.Errorf("line %d: invalid section header", lineNo)
			}
			section = strings.TrimSpace(line[1 : len(line)-1])
			if section == "" || strings.Contains(section, ".") {
				return nil, fmt.Errorf("line %d: invalid section name", lineNo)
			}
			continue
		}

		key, raw, ok := strings.Cut(line, "=")
		if !ok {
			return nil, fmt.Errorf("line %d: missing '='", lineNo)
		}
		key = strings.TrimSpace(key)
		raw = strings.TrimSpace(raw)
		if key == "" {
			return nil, fmt.Errorf("line %d: empty key", lineNo)
		}

		fullKey := key
		if section != "" {
			fullKey = section + "." + key
		}
		values[fullKey] = raw
	}
	if err := scanner.Err(); err != nil {
		return nil, err
	}
	return values, nil
}

func stripInlineComment(line string) string {
	var builder strings.Builder
	inQuotes := false
	escaped := false

	for _, r := range line {
		switch {
		case escaped:
			builder.WriteRune(r)
			escaped = false
		case r == '\\' && inQuotes:
			builder.WriteRune(r)
			escaped = true
		case r == '"':
			builder.WriteRune(r)
			inQuotes = !inQuotes
		case r == '#' && !inQuotes:
			return builder.String()
		default:
			builder.WriteRune(r)
		}
	}
	return builder.String()
}

func getString(values map[string]string, key, fallback string) string {
	raw, ok := values[key]
	if !ok {
		return fallback
	}
	if unquoted, err := strconv.Unquote(raw); err == nil {
		return unquoted
	}
	return strings.TrimSpace(strings.Trim(raw, `"`))
}

func getUint32(values map[string]string, key string, fallback uint32) uint32 {
	raw, ok := values[key]
	if !ok {
		return fallback
	}
	value, err := strconv.ParseUint(strings.TrimSpace(raw), 10, 32)
	if err != nil {
		return fallback
	}
	return uint32(value)
}

func generateRandomSecret(length int) (string, error) {
	if length <= 0 {
		return "", fmt.Errorf("secret length must be > 0")
	}

	raw := make([]byte, length)
	if _, err := rand.Read(raw); err != nil {
		return "", err
	}

	out := make([]byte, length)
	for i, b := range raw {
		out[i] = randomSecretAlphabet[int(b)%len(randomSecretAlphabet)]
	}
	return string(out), nil
}
