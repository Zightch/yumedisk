package filestorage

import (
	"errors"
	"os"
	"path/filepath"
	"testing"
)

func TestOpenRawBackendProbesSizeAndReadsWrites(t *testing.T) {
	t.Parallel()

	tempDir := t.TempDir()
	path := filepath.Join(tempDir, "disk.raw")
	initial := []byte("0123456789")
	if err := os.WriteFile(path, initial, 0o644); err != nil {
		t.Fatalf("write raw file: %v", err)
	}

	backend, err := Open(path, false)
	if err != nil {
		t.Fatalf("open backend: %v", err)
	}
	t.Cleanup(func() { _ = backend.Close() })

	if got := backend.Size(); got != uint64(len(initial)) {
		t.Fatalf("size mismatch: got %d want %d", got, len(initial))
	}

	buf := make([]byte, 4)
	if err := backend.ReadAt(2, buf); err != nil {
		t.Fatalf("read at: %v", err)
	}
	if string(buf) != "2345" {
		t.Fatalf("read mismatch: %q", string(buf))
	}

	if err := backend.WriteAt(5, []byte("AB")); err != nil {
		t.Fatalf("write at: %v", err)
	}

	updated, err := os.ReadFile(path)
	if err != nil {
		t.Fatalf("read updated file: %v", err)
	}
	if string(updated) != "01234AB789" {
		t.Fatalf("updated content mismatch: %q", string(updated))
	}
}

func TestRawBackendRejectsOutOfRangeAndReadOnlyWrites(t *testing.T) {
	t.Parallel()

	tempDir := t.TempDir()
	path := filepath.Join(tempDir, "disk.raw")
	if err := os.WriteFile(path, []byte("0123456789"), 0o644); err != nil {
		t.Fatalf("write raw file: %v", err)
	}

	backend, err := Open(path, true)
	if err != nil {
		t.Fatalf("open read-only backend: %v", err)
	}
	t.Cleanup(func() { _ = backend.Close() })

	if err := backend.ReadAt(9, make([]byte, 2)); !errors.Is(err, ErrOutOfRange) {
		t.Fatalf("expected out-of-range error, got %v", err)
	}
	if err := backend.WriteAt(0, []byte("X")); !errors.Is(err, ErrReadOnly) {
		t.Fatalf("expected read-only error, got %v", err)
	}
}
