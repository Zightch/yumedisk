package clientauth

import (
	"sync"
	"sync/atomic"
	"time"

	"yumedisk/server/internal/proto"
)

type Grant struct {
	ID               uint64
	ClientConnection uint64
	DiskID           string
	ExpiresAt        time.Time
}

type Registry struct {
	nextID atomic.Uint64
	now    func() time.Time

	mu     sync.RWMutex
	items  map[uint64]Grant
	byConn map[uint64]map[uint64]struct{}
	byDisk map[string]map[uint64]struct{}
}

func NewRegistry() *Registry {
	return &Registry{
		now:    time.Now,
		items:  make(map[uint64]Grant),
		byConn: make(map[uint64]map[uint64]struct{}),
		byDisk: make(map[string]map[uint64]struct{}),
	}
}

func (r *Registry) Issue(connectionID uint64, diskID string, expiresAt time.Time) uint64 {
	id := r.nextID.Add(1)
	if id == 0 {
		id = r.nextID.Add(1)
	}

	item := Grant{
		ID:               id,
		ClientConnection: connectionID,
		DiskID:           diskID,
		ExpiresAt:        expiresAt,
	}

	r.mu.Lock()
	r.items[id] = item
	if _, ok := r.byConn[connectionID]; !ok {
		r.byConn[connectionID] = make(map[uint64]struct{})
	}
	r.byConn[connectionID][id] = struct{}{}
	if _, ok := r.byDisk[diskID]; !ok {
		r.byDisk[diskID] = make(map[uint64]struct{})
	}
	r.byDisk[diskID][id] = struct{}{}
	r.mu.Unlock()
	return id
}

func (r *Registry) Lookup(id uint64, connectionID uint64) (Grant, uint16, bool) {
	r.mu.Lock()
	defer r.mu.Unlock()

	item, ok := r.items[id]
	if !ok || item.ClientConnection != connectionID {
		return Grant{}, proto.StatusAuthIDInvalid, false
	}
	if !item.ExpiresAt.After(r.now()) {
		r.removeLocked(id)
		return Grant{}, proto.StatusAuthIDExpired, false
	}
	return item, proto.StatusOK, true
}

func (r *Registry) Consume(id uint64) (Grant, bool) {
	r.mu.Lock()
	defer r.mu.Unlock()

	item, ok := r.items[id]
	if !ok {
		return Grant{}, false
	}
	r.removeLocked(id)
	return item, true
}

func (r *Registry) LookupDisk(id uint64, connectionID uint64) (string, uint16, bool) {
	item, status, ok := r.Lookup(id, connectionID)
	if !ok {
		return "", status, false
	}
	return item.DiskID, status, true
}

func (r *Registry) ConsumeDisk(id uint64) (string, bool) {
	item, ok := r.Consume(id)
	if !ok {
		return "", false
	}
	return item.DiskID, true
}

func (r *Registry) CloseConnection(connectionID uint64) {
	r.mu.Lock()
	for _, id := range r.collectConnectionIDsLocked(connectionID) {
		r.removeLocked(id)
	}
	r.mu.Unlock()
}

func (r *Registry) CloseDisk(diskID string) {
	r.mu.Lock()
	for _, id := range r.collectDiskIDsLocked(diskID) {
		r.removeLocked(id)
	}
	r.mu.Unlock()
}

func (r *Registry) collectConnectionIDsLocked(connectionID uint64) []uint64 {
	owned := r.byConn[connectionID]
	if len(owned) == 0 {
		return nil
	}

	ids := make([]uint64, 0, len(owned))
	for id := range owned {
		ids = append(ids, id)
	}
	return ids
}

func (r *Registry) collectDiskIDsLocked(diskID string) []uint64 {
	owned := r.byDisk[diskID]
	if len(owned) == 0 {
		return nil
	}

	ids := make([]uint64, 0, len(owned))
	for id := range owned {
		ids = append(ids, id)
	}
	return ids
}

func (r *Registry) removeLocked(id uint64) {
	item, ok := r.items[id]
	if !ok {
		return
	}
	delete(r.items, id)
	r.removeConnectionIndexLocked(item.ClientConnection, id)
	r.removeDiskIndexLocked(item.DiskID, id)
}

func (r *Registry) removeConnectionIndexLocked(connectionID uint64, id uint64) {
	owned, ok := r.byConn[connectionID]
	if !ok {
		return
	}
	delete(owned, id)
	if len(owned) == 0 {
		delete(r.byConn, connectionID)
	}
}

func (r *Registry) removeDiskIndexLocked(diskID string, id uint64) {
	owned, ok := r.byDisk[diskID]
	if !ok {
		return
	}
	delete(owned, id)
	if len(owned) == 0 {
		delete(r.byDisk, diskID)
	}
}
