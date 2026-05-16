package config

import (
	"bufio"
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
	DefaultListenAddr      = "127.0.0.1:9736"
	DefaultStorageFilePath = "data/disk.img"
	DefaultClaimSecretLen  = 64
	DefaultConfigFileName  = "config.toml"
)

type Config struct {
	ListenAddr      string
	StorageFilePath string
	ClaimCode       string
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

func LoadOrInit(path string, stdin io.Reader, stdout io.Writer) (Config, bool, error) {
	cfg, err := Load(path)
	if err == nil {
		return cfg, false, nil
	}
	if !errors.Is(err, fs.ErrNotExist) {
		return Config{}, false, err
	}

	cfg, err = promptAndCreate(path, stdin, stdout)
	if err != nil {
		return Config{}, false, err
	}
	return cfg, true, nil
}

func Load(path string) (Config, error) {
	data, err := os.ReadFile(path)
	if err != nil {
		return Config{}, err
	}

	values, err := parseFlatTOML(string(data))
	if err != nil {
		return Config{}, fmt.Errorf("parse %s: %w", path, err)
	}

	cfg := Config{
		ListenAddr:      getString(values, "listen_addr", DefaultListenAddr),
		StorageFilePath: getString(values, "storage_file_path", DefaultStorageFilePath),
		ClaimCode:       getString(values, "claim_code", ""),
	}
	if err := cfg.Validate(); err != nil {
		return Config{}, err
	}
	return cfg, nil
}

func Save(path string, cfg Config) error {
	if err := cfg.Validate(); err != nil {
		return err
	}
	if err := os.MkdirAll(filepath.Dir(path), 0o755); err != nil {
		return fmt.Errorf("create config directory: %w", err)
	}

	content := fmt.Sprintf(
		"listen_addr = %q\nstorage_file_path = %q\nclaim_code = %q\n",
		cfg.ListenAddr,
		cfg.StorageFilePath,
		cfg.ClaimCode,
	)
	if err := os.WriteFile(path, []byte(content), 0o644); err != nil {
		return fmt.Errorf("write config file: %w", err)
	}
	return nil
}

func (c Config) Validate() error {
	if strings.TrimSpace(c.ListenAddr) == "" {
		return errors.New("listen_addr must not be empty")
	}
	if strings.TrimSpace(c.StorageFilePath) == "" {
		return errors.New("storage_file_path must not be empty")
	}
	if _, err := auth.ParseClaimCode(c.ClaimCode); err != nil {
		return fmt.Errorf("invalid claim_code: %w", err)
	}
	return nil
}

func promptAndCreate(path string, stdin io.Reader, stdout io.Writer) (Config, error) {
	reader := bufio.NewReader(stdin)
	listenAddr, err := promptLine(reader, stdout, "listen_addr", DefaultListenAddr)
	if err != nil {
		return Config{}, err
	}
	storageFilePath, err := promptLine(reader, stdout, "storage_file_path", DefaultStorageFilePath)
	if err != nil {
		return Config{}, err
	}

	claimCode, err := auth.GenerateClaimCode(DefaultClaimSecretLen)
	if err != nil {
		return Config{}, fmt.Errorf("generate claim code: %w", err)
	}

	cfg := Config{
		ListenAddr:      listenAddr,
		StorageFilePath: storageFilePath,
		ClaimCode:       claimCode,
	}
	if err := Save(path, cfg); err != nil {
		return Config{}, err
	}

	fmt.Fprintf(stdout, "generated claim_code = %s\n", cfg.ClaimCode)
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

func parseFlatTOML(content string) (map[string]string, error) {
	values := make(map[string]string)
	scanner := bufio.NewScanner(strings.NewReader(content))
	lineNo := 0
	for scanner.Scan() {
		lineNo++
		line := strings.TrimSpace(scanner.Text())
		if line == "" || strings.HasPrefix(line, "#") {
			continue
		}

		key, raw, ok := strings.Cut(line, "=")
		if !ok {
			return nil, fmt.Errorf("line %d: missing '='", lineNo)
		}
		key = strings.TrimSpace(key)
		raw = strings.TrimSpace(raw)
		values[key] = raw
	}
	if err := scanner.Err(); err != nil {
		return nil, err
	}
	return values, nil
}

func getString(values map[string]string, key, fallback string) string {
	raw, ok := values[key]
	if !ok {
		return fallback
	}
	if unquoted, err := strconv.Unquote(raw); err == nil {
		return unquoted
	}
	return strings.Trim(raw, `"`)
}

func getUint32(values map[string]string, key string, fallback uint32) uint32 {
	raw, ok := values[key]
	if !ok {
		return fallback
	}
	value, err := strconv.ParseUint(raw, 10, 32)
	if err != nil {
		return fallback
	}
	return uint32(value)
}
