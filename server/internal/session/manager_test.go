package session

import "testing"

func TestManagerCloseWaitsForInflightIOBeforeDrain(t *testing.T) {
	manager := NewManager()

	record, err := manager.Open(Record{
		Connection: 1,
		Metadata: Metadata{
			DiskID:        "A1b2C3d4E5f6G7h8",
			DiskSizeBytes: 4096,
			MaxIOBytes:    1024,
		},
	})
	if err != nil {
		t.Fatalf("open session: %v", err)
	}

	acquired, lease, err := manager.beginIO(record.ID)
	if err != nil {
		t.Fatalf("begin io: %v", err)
	}
	if acquired.Closing {
		t.Fatal("expected acquired session to be live")
	}

	manager.Close(record.ID)

	closingRecord, ok := manager.Get(record.ID)
	if !ok {
		t.Fatal("expected closing session to stay registered until drain")
	}
	if !closingRecord.Closing {
		t.Fatal("expected session to be marked closing")
	}

	if _, err := manager.Open(Record{Connection: 2}); err != ErrSessionOpenRejected {
		t.Fatalf("expected open rejected while closing session is draining, got %v", err)
	}
	if _, _, err := manager.beginIO(record.ID); err != ErrSessionUnavailable {
		t.Fatalf("expected new io to be rejected after close, got %v", err)
	}

	lease.release()

	if _, ok := manager.Get(record.ID); ok {
		t.Fatal("expected drained session to be removed")
	}
	reopened, err := manager.Open(Record{Connection: 2})
	if err != nil {
		t.Fatalf("reopen after drain: %v", err)
	}
	if reopened.ID == 0 {
		t.Fatal("expected non-zero reopened session id")
	}
}

func TestManagerCloseConnectionWaitsForInflightIOBeforeDrain(t *testing.T) {
	manager := NewManager()

	record, err := manager.Open(Record{
		Connection: 7,
		Metadata: Metadata{
			DiskID:        "A1b2C3d4E5f6G7h8",
			DiskSizeBytes: 4096,
			MaxIOBytes:    1024,
		},
	})
	if err != nil {
		t.Fatalf("open session: %v", err)
	}

	_, lease, err := manager.beginIO(record.ID)
	if err != nil {
		t.Fatalf("begin io: %v", err)
	}

	manager.CloseConnection(7)

	closingRecord, ok := manager.Get(record.ID)
	if !ok {
		t.Fatal("expected connection-closed session to stay registered until drain")
	}
	if !closingRecord.Closing {
		t.Fatal("expected session to be marked closing after connection close")
	}

	if _, err := manager.Open(Record{Connection: 8}); err != ErrSessionOpenRejected {
		t.Fatalf("expected open rejected while connection-close drain is in progress, got %v", err)
	}

	lease.release()

	if _, ok := manager.Get(record.ID); ok {
		t.Fatal("expected drained connection-closed session to be removed")
	}
	if _, err := manager.Open(Record{Connection: 8}); err != nil {
		t.Fatalf("open after connection-close drain: %v", err)
	}
}
