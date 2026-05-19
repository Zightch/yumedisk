package gateway

import "testing"

func TestLocalAdapterRegistersSingleWholeRoute(t *testing.T) {
	t.Parallel()

	core := newGatewayTestCore(t)
	backend, err := NewLocalAdapter(core)
	if err != nil {
		t.Fatalf("NewLocalAdapter returned error: %v", err)
	}

	entry, ok := backend.LookupRoute(core.DiskID())
	if !ok {
		t.Fatal("expected whole route to be registered")
	}
	if entry.ConnectionID != WholeRouteConnectionID {
		t.Fatalf("unexpected whole route connection id: %d", entry.ConnectionID)
	}
	if entry.RouteTarget != "embedded://whole" {
		t.Fatalf("unexpected whole route target: %s", entry.RouteTarget)
	}
	if _, ok := backend.LookupRoute("DISK000000000000"); ok {
		t.Fatal("unexpected unrelated whole route")
	}
}
