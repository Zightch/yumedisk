package session

import (
	"os"
	"path/filepath"
	"testing"

	filestorage "yumedisk/server/internal/storage/file"
)

func TestReadAndWriteKeepSessionOpen(t *testing.T) {
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
	})
	desc, err := service.Open(1)
	if err != nil {
		t.Fatalf("open session: %v", err)
	}

	if err := service.Write(desc.ID, 0, []byte("ABCD")); err != nil {
		t.Fatalf("write: %v", err)
	}
	if _, err := service.Read(desc.ID, 0, 4); err != nil {
		t.Fatalf("read: %v", err)
	}

	record, ok := service.Manager().Get(desc.ID)
	if !ok {
		t.Fatalf("session missing after read/write")
	}
	if record.Connection != 1 {
		t.Fatalf("unexpected session connection: %d", record.Connection)
	}
}

func TestCloseMakesSessionUnavailable(t *testing.T) {
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
	})
	desc, err := service.Open(1)
	if err != nil {
		t.Fatalf("open session: %v", err)
	}

	service.Close(desc.ID)

	if _, err := service.Read(desc.ID, 0, 4); err != ErrSessionUnavailable {
		t.Fatalf("expected session unavailable after close, got %v", err)
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
	})
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
