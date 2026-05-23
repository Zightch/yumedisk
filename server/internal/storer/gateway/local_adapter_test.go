package gateway

import (
	"os"
	"path/filepath"
	"testing"

	"yumedisk/server/internal/route"
	"yumedisk/server/internal/session"
	filestorage "yumedisk/server/internal/storage/file"
)

func TestLocalAdapterRegistersWholeRoutesForAllExports(t *testing.T) {
	t.Parallel()

	rwExport, roExport := newGatewayTestExports(t)
	backend, err := NewLocalAdapter([]LocalExport{rwExport, roExport})
	if err != nil {
		t.Fatalf("NewLocalAdapter returned error: %v", err)
	}

	rwEntry, ok := backend.LookupRoute(rwExport.DiskID())
	if !ok {
		t.Fatal("expected rw route to be registered")
	}
	roEntry, ok := backend.LookupRoute(roExport.DiskID())
	if !ok {
		t.Fatal("expected ro route to be registered")
	}
	if rwEntry.ConnectionID == roEntry.ConnectionID {
		t.Fatalf("expected distinct route connection ids, got %d", rwEntry.ConnectionID)
	}
	if rwEntry.RouteTarget != "embedded://whole/"+rwExport.DiskID() {
		t.Fatalf("unexpected rw route target: %s", rwEntry.RouteTarget)
	}
	if roEntry.RouteTarget != "embedded://whole/"+roExport.DiskID() {
		t.Fatalf("unexpected ro route target: %s", roEntry.RouteTarget)
	}
	if _, ok := backend.LookupRoute("DISK000000000000"); ok {
		t.Fatal("unexpected unrelated whole route")
	}
}

func TestLocalAdapterDispatchesByRouteConnection(t *testing.T) {
	t.Parallel()

	rwExport, roExport := newGatewayTestExports(t)
	backend, err := NewLocalAdapter([]LocalExport{rwExport, roExport})
	if err != nil {
		t.Fatalf("NewLocalAdapter returned error: %v", err)
	}

	rwEntry, ok := backend.LookupRoute(rwExport.DiskID())
	if !ok {
		t.Fatal("expected rw route")
	}
	roEntry, ok := backend.LookupRoute(roExport.DiskID())
	if !ok {
		t.Fatal("expected ro route")
	}

	rwSessionID, err := backend.Open(10, rwEntry)
	if err != nil {
		t.Fatalf("open rw session: %v", err)
	}
	roSessionID, err := backend.Open(20, roEntry)
	if err != nil {
		t.Fatalf("open ro session: %v", err)
	}

	if err := backend.Write(rwEntry.ConnectionID, rwSessionID, 8, []byte("YUME")); err != nil {
		t.Fatalf("rw write: %v", err)
	}
	data, err := backend.Read(roEntry.ConnectionID, roSessionID, 8, 4)
	if err != nil {
		t.Fatalf("ro read: %v", err)
	}
	if string(data) != "YUME" {
		t.Fatalf("unexpected ro read payload: %q", string(data))
	}
	if err := backend.Write(roEntry.ConnectionID, roSessionID, 8, []byte("FAIL")); err != session.ErrReadOnly {
		t.Fatalf("expected ro write to be read only, got %v", err)
	}
}

func newGatewayTestExports(t *testing.T) (*gatewayTestExport, *gatewayTestExport) {
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

	rwMetadata := session.Metadata{
		DiskID:        "DISK000000000001",
		DiskSizeBytes: 4096,
		ReadOnly:      false,
		MaxIOBytes:    60 * 1024,
	}
	roMetadata := session.Metadata{
		DiskID:        "DISK000000000002",
		DiskSizeBytes: 4096,
		ReadOnly:      true,
		MaxIOBytes:    60 * 1024,
	}
	return &gatewayTestExport{
			sessions: session.NewService(session.NewExclusiveManager(), storage, rwMetadata),
			metadata: rwMetadata,
		}, &gatewayTestExport{
			sessions: session.NewService(session.NewSharedManager(), storage, roMetadata),
			metadata: roMetadata,
		}
}

type gatewayTestExport struct {
	sessions *session.Service
	metadata session.Metadata
}

func (c *gatewayTestExport) DiskID() string {
	return c.metadata.DiskID
}

func (c *gatewayTestExport) SessionService() *session.Service {
	return c.sessions
}

func (c *gatewayTestExport) RouteEntry(routeTarget string, connectionID uint64) route.Entry {
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
