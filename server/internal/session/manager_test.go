package session

import "testing"

func TestExclusiveManagerCloseWaitsForInflightIOBeforeDrain(t *testing.T) {
	manager := NewExclusiveManager()

	record, err := manager.Open(Record{
		Connection: 1,
		Metadata: Metadata{
			DiskID:        "A1b2C3d4E5f6G7h8",
			DiskSizeBytes: 4096,
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

func TestExclusiveManagerCloseConnectionWaitsForInflightIOBeforeDrain(t *testing.T) {
	manager := NewExclusiveManager()

	record, err := manager.Open(Record{
		Connection: 7,
		Metadata: Metadata{
			DiskID:        "A1b2C3d4E5f6G7h8",
			DiskSizeBytes: 4096,
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

func TestSharedManagerAllowsMultipleLiveSessions(t *testing.T) {
	manager := NewSharedManager()

	first, err := manager.Open(Record{
		Connection: 1,
		Metadata: Metadata{
			DiskID:        "A1b2C3d4E5f6G7h8",
			DiskSizeBytes: 4096,
		},
	})
	if err != nil {
		t.Fatalf("open first session: %v", err)
	}
	second, err := manager.Open(Record{
		Connection: 2,
		Metadata: Metadata{
			DiskID:        "A1b2C3d4E5f6G7h8",
			DiskSizeBytes: 4096,
		},
	})
	if err != nil {
		t.Fatalf("open second session: %v", err)
	}
	if first.ID == 0 || second.ID == 0 || first.ID == second.ID {
		t.Fatalf("unexpected session ids: first=%d second=%d", first.ID, second.ID)
	}
}

func TestSharedManagerCloseOnlyAffectsTargetSession(t *testing.T) {
	manager := NewSharedManager()

	first, err := manager.Open(Record{
		Connection: 1,
		Metadata: Metadata{
			DiskID:        "A1b2C3d4E5f6G7h8",
			DiskSizeBytes: 4096,
		},
	})
	if err != nil {
		t.Fatalf("open first session: %v", err)
	}
	second, err := manager.Open(Record{
		Connection: 2,
		Metadata: Metadata{
			DiskID:        "A1b2C3d4E5f6G7h8",
			DiskSizeBytes: 4096,
		},
	})
	if err != nil {
		t.Fatalf("open second session: %v", err)
	}

	manager.Close(first.ID)

	if _, ok := manager.Get(first.ID); ok {
		t.Fatal("expected first session to close")
	}
	if _, ok := manager.Get(second.ID); !ok {
		t.Fatal("expected second session to remain open")
	}
}

func TestSharedManagerCloseConnectionOnlyAffectsMatchingSessions(t *testing.T) {
	manager := NewSharedManager()

	first, err := manager.Open(Record{
		Connection: 7,
		Metadata: Metadata{
			DiskID:        "A1b2C3d4E5f6G7h8",
			DiskSizeBytes: 4096,
		},
	})
	if err != nil {
		t.Fatalf("open first session: %v", err)
	}
	second, err := manager.Open(Record{
		Connection: 7,
		Metadata: Metadata{
			DiskID:        "A1b2C3d4E5f6G7h8",
			DiskSizeBytes: 4096,
		},
	})
	if err != nil {
		t.Fatalf("open second session: %v", err)
	}
	third, err := manager.Open(Record{
		Connection: 8,
		Metadata: Metadata{
			DiskID:        "A1b2C3d4E5f6G7h8",
			DiskSizeBytes: 4096,
		},
	})
	if err != nil {
		t.Fatalf("open third session: %v", err)
	}

	manager.CloseConnection(7)

	if _, ok := manager.Get(first.ID); ok {
		t.Fatal("expected first session to close with connection 7")
	}
	if _, ok := manager.Get(second.ID); ok {
		t.Fatal("expected second session to close with connection 7")
	}
	if _, ok := manager.Get(third.ID); !ok {
		t.Fatal("expected third session to remain open")
	}
}
