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

	service := NewService(NewExclusiveManager(), storage, Metadata{
		DiskID:        "A1b2C3d4E5f6G7h8",
		DiskSizeBytes: storage.Size(),
		ReadOnly:      storage.ReadOnly(),
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

	service := NewService(NewExclusiveManager(), storage, Metadata{
		DiskID:        "A1b2C3d4E5f6G7h8",
		DiskSizeBytes: storage.Size(),
		ReadOnly:      storage.ReadOnly(),
	})
	desc, err := service.Open(1)
	if err != nil {
		t.Fatalf("open session: %v", err)
	}

	service.Close(desc.ID)

	if _, err := service.Read(desc.ID, 0, 4); err != ErrSessionUnavailable {
		t.Fatalf("expected session unavailable after close, got %v", err)
	}
	if _, err := service.Open(2); err != nil {
		t.Fatalf("expected reopen to succeed after immediate close, got %v", err)
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

	service := NewService(NewExclusiveManager(), storage, Metadata{
		DiskID:        "A1b2C3d4E5f6G7h8",
		DiskSizeBytes: storage.Size(),
		ReadOnly:      storage.ReadOnly(),
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

func TestServiceCloseKeepsWriterSlotOccupiedUntilInflightIODrains(t *testing.T) {
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

	service := NewService(NewExclusiveManager(), storage, Metadata{
		DiskID:        "A1b2C3d4E5f6G7h8",
		DiskSizeBytes: storage.Size(),
		ReadOnly:      storage.ReadOnly(),
	})
	desc, err := service.Open(1)
	if err != nil {
		t.Fatalf("open session: %v", err)
	}

	record, lease, err := service.Manager().beginIO(desc.ID)
	if err != nil {
		t.Fatalf("begin io: %v", err)
	}
	if record.Closing {
		t.Fatal("expected session to be live before close")
	}

	service.Close(desc.ID)

	closingRecord, ok := service.Manager().Get(desc.ID)
	if !ok {
		t.Fatal("expected closing session to remain registered until drain")
	}
	if !closingRecord.Closing {
		t.Fatal("expected session to be marked closing")
	}
	if _, err := service.Open(2); err != ErrSessionOpenRejected {
		t.Fatalf("expected open rejected during drain, got %v", err)
	}
	if _, err := service.Read(desc.ID, 0, 4); err != ErrSessionUnavailable {
		t.Fatalf("expected new io rejected while closing, got %v", err)
	}

	lease.release()

	if _, ok := service.Manager().Get(desc.ID); ok {
		t.Fatal("expected session to be removed after drain")
	}
	if _, err := service.Open(2); err != nil {
		t.Fatalf("expected reopen after drain to succeed, got %v", err)
	}
}

func TestServiceAllowsMaxRawIoBytesButRejectsLargerRead(t *testing.T) {
	tempDir := t.TempDir()
	storagePath := filepath.Join(tempDir, "disk.img")
	file, err := os.Create(storagePath)
	if err != nil {
		t.Fatalf("create storage file: %v", err)
	}
	if err := file.Truncate(128 * 1024); err != nil {
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

	service := NewService(NewExclusiveManager(), storage, Metadata{
		DiskID:        "A1b2C3d4E5f6G7h8",
		DiskSizeBytes: storage.Size(),
		ReadOnly:      storage.ReadOnly(),
	})
	desc, err := service.Open(1)
	if err != nil {
		t.Fatalf("open session: %v", err)
	}

	if _, err := service.Read(desc.ID, 0, MaxDataPlaneRawBytes); err != nil {
		t.Fatalf("read at raw limit: %v", err)
	}
	if _, err := service.Read(desc.ID, 0, MaxDataPlaneRawBytes+1); err != ErrIOLimit {
		t.Fatalf("expected read above raw limit to fail with ErrIOLimit, got %v", err)
	}
}

func TestServiceAllowsMaxRawIoBytesButRejectsLargerWrite(t *testing.T) {
	tempDir := t.TempDir()
	storagePath := filepath.Join(tempDir, "disk.img")
	file, err := os.Create(storagePath)
	if err != nil {
		t.Fatalf("create storage file: %v", err)
	}
	if err := file.Truncate(128 * 1024); err != nil {
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

	service := NewService(NewExclusiveManager(), storage, Metadata{
		DiskID:        "A1b2C3d4E5f6G7h8",
		DiskSizeBytes: storage.Size(),
		ReadOnly:      storage.ReadOnly(),
	})
	desc, err := service.Open(1)
	if err != nil {
		t.Fatalf("open session: %v", err)
	}

	if err := service.Write(desc.ID, 0, make([]byte, MaxDataPlaneRawBytes)); err != nil {
		t.Fatalf("write at raw limit: %v", err)
	}
	if err := service.Write(desc.ID, 0, make([]byte, MaxDataPlaneRawBytes+1)); err != ErrIOLimit {
		t.Fatalf("expected write above raw limit to fail with ErrIOLimit, got %v", err)
	}
}
