package client

import (
	"sync"
	"sync/atomic"
	"time"

	"yumedisk/server/internal/proto"
)

type authGrant struct {
	ID               uint64
	ClientConnection uint64
	DiskID           string
	ExpiresAt        time.Time
}

type authGrantRegistry struct {
	nextID atomic.Uint64
	now    func() time.Time

	mu     sync.RWMutex
	items  map[uint64]authGrant
	byConn map[uint64]map[uint64]struct{}
	byDisk map[string]map[uint64]struct{}
}

func newAuthGrantRegistry() *authGrantRegistry {
	return &authGrantRegistry{
		now:    time.Now,
		items:  make(map[uint64]authGrant),
		byConn: make(map[uint64]map[uint64]struct{}),
		byDisk: make(map[string]map[uint64]struct{}),
	}
}

func (r *authGrantRegistry) Issue(connectionID uint64, diskID string, expiresAt time.Time) uint64 {
	id := r.nextID.Add(1)
	if id == 0 {
		id = r.nextID.Add(1)
	}

	item := authGrant{
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

func (r *authGrantRegistry) Lookup(id uint64, connectionID uint64) (authGrant, uint16, bool) {
	r.mu.Lock()
	defer r.mu.Unlock()

	item, ok := r.items[id]
	if !ok || item.ClientConnection != connectionID {
		return authGrant{}, proto.StatusAuthIDInvalid, false
	}
	if !item.ExpiresAt.After(r.now()) {
		r.removeLocked(id)
		return authGrant{}, proto.StatusAuthIDExpired, false
	}
	return item, proto.StatusOK, true
}

func (r *authGrantRegistry) Consume(id uint64) (authGrant, bool) {
	r.mu.Lock()
	defer r.mu.Unlock()

	item, ok := r.items[id]
	if !ok {
		return authGrant{}, false
	}
	r.removeLocked(id)
	return item, true
}

func (r *authGrantRegistry) LookupDisk(id uint64, connectionID uint64) (string, uint16, bool) {
	item, status, ok := r.Lookup(id, connectionID)
	if !ok {
		return "", status, false
	}
	return item.DiskID, status, true
}

func (r *authGrantRegistry) ConsumeDisk(id uint64) (string, bool) {
	item, ok := r.Consume(id)
	if !ok {
		return "", false
	}
	return item.DiskID, true
}

func (r *authGrantRegistry) CloseConnection(connectionID uint64) {
	r.mu.Lock()
	for _, id := range r.collectConnectionIDsLocked(connectionID) {
		r.removeLocked(id)
	}
	r.mu.Unlock()
}

func (r *authGrantRegistry) CloseDisk(diskID string) {
	r.mu.Lock()
	for _, id := range r.collectDiskIDsLocked(diskID) {
		r.removeLocked(id)
	}
	r.mu.Unlock()
}

func (r *authGrantRegistry) collectConnectionIDsLocked(connectionID uint64) []uint64 {
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

func (r *authGrantRegistry) collectDiskIDsLocked(diskID string) []uint64 {
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

func (r *authGrantRegistry) removeLocked(id uint64) {
	item, ok := r.items[id]
	if !ok {
		return
	}
	delete(r.items, id)
	r.removeConnectionIndexLocked(item.ClientConnection, id)
	r.removeDiskIndexLocked(item.DiskID, id)
}

func (r *authGrantRegistry) removeConnectionIndexLocked(connectionID uint64, id uint64) {
	owned, ok := r.byConn[connectionID]
	if !ok {
		return
	}
	delete(owned, id)
	if len(owned) == 0 {
		delete(r.byConn, connectionID)
	}
}

func (r *authGrantRegistry) removeDiskIndexLocked(diskID string, id uint64) {
	owned, ok := r.byDisk[diskID]
	if !ok {
		return
	}
	delete(owned, id)
	if len(owned) == 0 {
		delete(r.byDisk, diskID)
	}
}
