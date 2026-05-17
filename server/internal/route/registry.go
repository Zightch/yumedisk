package route

import (
	"errors"
	"sync"
)

var ErrDiskAlreadyRegistered = errors.New("disk id already registered on another connection")

type Entry struct {
	DiskID            string
	AuthVerifier      [64]byte
	RouteTarget       string
	ConnectionID      uint64
	Connected         bool
	DiskSizeBytes     uint64
	ReadOnly          bool
	MaxIOBytes        uint32
	SessionTTLSeconds uint32
}

type Registry struct {
	mu    sync.RWMutex
	items map[string]Entry
}

func NewRegistry() *Registry {
	return &Registry{
		items: make(map[string]Entry),
	}
}

func (r *Registry) Register(entry Entry) error {
	if entry.DiskID == "" {
		return errors.New("route entry requires disk id")
	}
	if entry.RouteTarget == "" {
		return errors.New("route entry requires route target")
	}
	if !entry.Connected {
		return errors.New("route entry must be connected")
	}

	r.mu.Lock()
	if existing, ok := r.items[entry.DiskID]; ok && existing.Connected && existing.ConnectionID != entry.ConnectionID {
		r.mu.Unlock()
		return ErrDiskAlreadyRegistered
	}
	r.items[entry.DiskID] = entry
	r.mu.Unlock()
	return nil
}

func (r *Registry) LookupRoute(diskID string) (Entry, bool) {
	r.mu.RLock()
	entry, ok := r.items[diskID]
	r.mu.RUnlock()
	if !ok || !entry.Connected {
		return Entry{}, false
	}
	return entry, true
}

func (r *Registry) DisconnectConnection(connectionID uint64) {
	r.mu.Lock()
	for diskID, entry := range r.items {
		if entry.ConnectionID != connectionID {
			continue
		}
		entry.Connected = false
		r.items[diskID] = entry
	}
	r.mu.Unlock()
}
