package route

import "testing"

func TestRegistryRegisterAndLookupRoute(t *testing.T) {
	t.Parallel()

	registry := NewRegistry()
	entry := Entry{
		DiskID:            "DISK000000000001",
		AuthVerifier:      [64]byte{1, 2, 3},
		RouteTarget:       "embedded://whole",
		ConnectionID:      7,
		Connected:         true,
		DiskSizeBytes:     4096,
		ReadOnly:          false,
		MaxIOBytes:        8192,
		SessionTTLSeconds: 30,
	}
	if err := registry.Register(entry); err != nil {
		t.Fatalf("register route: %v", err)
	}

	got, ok := registry.LookupRoute(entry.DiskID)
	if !ok {
		t.Fatal("expected route to be found")
	}
	if got.DiskID != entry.DiskID {
		t.Fatalf("unexpected disk id: %q", got.DiskID)
	}
	if got.RouteTarget != entry.RouteTarget {
		t.Fatalf("unexpected route target: %q", got.RouteTarget)
	}
	if got.AuthVerifier != entry.AuthVerifier {
		t.Fatal("unexpected auth verifier")
	}
	if got.ConnectionID != entry.ConnectionID {
		t.Fatalf("unexpected connection id: %d", got.ConnectionID)
	}
	if got.DiskSizeBytes != entry.DiskSizeBytes {
		t.Fatalf("unexpected disk size: %d", got.DiskSizeBytes)
	}
	if got.MaxIOBytes != entry.MaxIOBytes {
		t.Fatalf("unexpected max io bytes: %d", got.MaxIOBytes)
	}
	if got.SessionTTLSeconds != entry.SessionTTLSeconds {
		t.Fatalf("unexpected session ttl: %d", got.SessionTTLSeconds)
	}
}

func TestRegistryDisconnectConnectionHidesRoute(t *testing.T) {
	t.Parallel()

	registry := NewRegistry()
	if err := registry.Register(Entry{
		DiskID:       "DISK000000000001",
		AuthVerifier: [64]byte{9},
		RouteTarget:  "storer://127.0.0.1:9836",
		ConnectionID: 12,
		Connected:    true,
	}); err != nil {
		t.Fatalf("register route: %v", err)
	}

	registry.DisconnectConnection(12)

	if _, ok := registry.LookupRoute("DISK000000000001"); ok {
		t.Fatal("expected disconnected route to become unavailable")
	}
}
