package session

import (
	"os"
	"path/filepath"
	"testing"
	"time"

	filestorage "yumedisk/server/internal/storage/file"
)

func TestReadAndWriteRefreshSessionExpiration(t *testing.T) {
	tempDir := t.TempDir()
	storagePath := filepath.Join(tempDir, "disk.img")
	file, err := os.Create(storagePath)
	if err != nil {
		t.Fatalf("create storage file: %v", err)
	}
	if err := file.Truncate(4096); err != nil {
		_ = file.Close()
		t.Fatalf("truncate storage file: %v", err)
	}
	if err := file.Close(); err != nil {
		t.Fatalf("close storage file: %v", err)
	}

	storage, err := filestorage.Open(storagePath, false)
	if err != nil {
		t.Fatalf("open storage: %v", err)
	}
	t.Cleanup(func() { _ = storage.Close() })

	service := NewService(NewManager(), storage, Metadata{
		DiskID:        "A1b2C3d4E5f6G7h8",
		DiskSizeBytes: storage.Size(),
		ReadOnly:      storage.ReadOnly(),
		MaxIOBytes:    1024,
	}, 5*time.Second)
	desc, err := service.Open(1)
	if err != nil {
		t.Fatalf("open session: %v", err)
	}
	originalExpiresAt := desc.ExpiresAt

	time.Sleep(5 * time.Millisecond)
	if err := service.Write(desc.ID, 0, []byte("ABCD")); err != nil {
		t.Fatalf("write: %v", err)
	}
	afterWrite, ok := service.Manager().Get(desc.ID)
	if !ok {
		t.Fatalf("session missing after write")
	}
	if !afterWrite.ExpiresAt.After(originalExpiresAt) {
		t.Fatalf("write did not refresh expiration")
	}

	time.Sleep(5 * time.Millisecond)
	if _, err := service.Read(desc.ID, 0, 4); err != nil {
		t.Fatalf("read: %v", err)
	}
	afterRead, ok := service.Manager().Get(desc.ID)
	if !ok {
		t.Fatalf("session missing after read")
	}
	if !afterRead.ExpiresAt.After(afterWrite.ExpiresAt) {
		t.Fatalf("read did not refresh expiration")
	}
}

func TestOpenRejectsWhileSessionIsLive(t *testing.T) {
	tempDir := t.TempDir()
	storagePath := filepath.Join(tempDir, "disk.img")
	file, err := os.Create(storagePath)
	if err != nil {
		t.Fatalf("create storage file: %v", err)
	}
	if err := file.Truncate(4096); err != nil {
		_ = file.Close()
		t.Fatalf("truncate storage file: %v", err)
	}
	if err := file.Close(); err != nil {
		t.Fatalf("close storage file: %v", err)
	}

	storage, err := filestorage.Open(storagePath, false)
	if err != nil {
		t.Fatalf("open storage: %v", err)
	}
	t.Cleanup(func() { _ = storage.Close() })

	service := NewService(NewManager(), storage, Metadata{
		DiskID:        "A1b2C3d4E5f6G7h8",
		DiskSizeBytes: storage.Size(),
		ReadOnly:      storage.ReadOnly(),
		MaxIOBytes:    1024,
	}, 5*time.Second)
	first, err := service.Open(1)
	if err != nil {
		t.Fatalf("open first session: %v", err)
	}
	if first.ID == 0 {
		t.Fatal("expected non-zero session id")
	}

	_, err = service.Open(2)
	if err != ErrSessionOpenRejected {
		t.Fatalf("expected session open rejected, got %v", err)
	}
}
