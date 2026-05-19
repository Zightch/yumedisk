package gateway

import (
	"os"
	"path/filepath"
	"testing"

	"yumedisk/server/internal/route"
	"yumedisk/server/internal/session"
	filestorage "yumedisk/server/internal/storage/file"
)

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

func newGatewayTestCore(t *testing.T) *gatewayTestCore {
	t.Helper()

	tempDir := t.TempDir()
	rawPath := filepath.Join(tempDir, "disk.raw")
	if err := os.WriteFile(rawPath, make([]byte, 4096), 0o644); err != nil {
		t.Fatalf("write raw file: %v", err)
	}

	storage, err := filestorage.Open(rawPath, false)
	if err != nil {
		t.Fatalf("open storage: %v", err)
	}
	t.Cleanup(func() { _ = storage.Close() })

	metadata := session.Metadata{
		DiskID:        "DISK000000000001",
		DiskSizeBytes: 4096,
		ReadOnly:      false,
		MaxIOBytes:    60 * 1024,
	}
	return &gatewayTestCore{
		sessions: session.NewService(session.NewManager(), storage, metadata),
		metadata: metadata,
	}
}

type gatewayTestCore struct {
	sessions *session.Service
	metadata session.Metadata
}

func (c *gatewayTestCore) DiskID() string {
	return c.metadata.DiskID
}

func (c *gatewayTestCore) SessionService() *session.Service {
	return c.sessions
}

func (c *gatewayTestCore) RouteEntry(routeTarget string, connectionID uint64) route.Entry {
	return route.Entry{
		DiskID:        c.metadata.DiskID,
		RouteTarget:   routeTarget,
		ConnectionID:  connectionID,
		Connected:     true,
		DiskSizeBytes: c.metadata.DiskSizeBytes,
		ReadOnly:      c.metadata.ReadOnly,
		MaxIOBytes:    c.metadata.MaxIOBytes,
	}
}
