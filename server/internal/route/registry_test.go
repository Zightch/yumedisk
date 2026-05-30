package route

import "testing"

func TestRegistryRegisterAndLookupRoute(t *testing.T) {
	t.Parallel()

	registry := NewRegistry()
	entry := Entry{
		DiskID:       "DISK000000000001",
		AuthVerifier: [64]byte{1, 2, 3},
		RouteTarget:  "embedded://whole",
		ConnectionID: 7,
		Connected:    true,
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
}

func TestRegistryDisconnectConnectionHidesRouteAndReturnsEntries(t *testing.T) {
	t.Parallel()

	registry := NewRegistry()
	entry := Entry{
		DiskID:       "DISK000000000001",
		AuthVerifier: [64]byte{9},
		RouteTarget:  "storer://127.0.0.1:9836",
		ConnectionID: 12,
		Connected:    true,
	}
	if err := registry.Register(entry); err != nil {
		t.Fatalf("register route: %v", err)
	}

	disconnected := registry.DisconnectConnection(12)
	if len(disconnected) != 1 || disconnected[0].DiskID != entry.DiskID {
		t.Fatalf("unexpected disconnected entries: %+v", disconnected)
	}

	if _, ok := registry.LookupRoute(entry.DiskID); ok {
		t.Fatal("expected disconnected route to become unavailable")
	}
}

func TestRegistryRejectsDuplicateDiskOnAnotherConnection(t *testing.T) {
	t.Parallel()

	registry := NewRegistry()
	entry := Entry{
		DiskID:       "DISK000000000001",
		AuthVerifier: [64]byte{1},
		RouteTarget:  "storer://127.0.0.1:9836",
		ConnectionID: 12,
		Connected:    true,
	}
	if err := registry.Register(entry); err != nil {
		t.Fatalf("register first route: %v", err)
	}

	err := registry.Register(Entry{
		DiskID:       entry.DiskID,
		AuthVerifier: [64]byte{2},
		RouteTarget:  "storer://127.0.0.1:9837",
		ConnectionID: 13,
		Connected:    true,
	})
	if err != ErrDiskAlreadyRegistered {
		t.Fatalf("expected duplicate disk error, got: %v", err)
	}
}
