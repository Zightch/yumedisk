package gateway

import (
	"sync"
	"sync/atomic"
)

const gatewaySessionBaseID = uint64(1) << 63

type gatewaySessionRuntime struct {
	ClientConnectionID uint64
	RouteConnectionID  uint64
	UpstreamSessionID  uint64
}

type gatewaySessionSnapshot struct {
	DiskID        string
	DiskSizeBytes uint64
	ReadOnly      bool
	MaxIOBytes    uint32
}

type gatewaySessionRecord struct {
	ID       uint64
	Runtime  gatewaySessionRuntime
	Snapshot gatewaySessionSnapshot
}

type sessionRegistry struct {
	nextID atomic.Uint64

	mu      sync.RWMutex
	items   map[uint64]gatewaySessionRecord
	byConn  map[uint64]map[uint64]struct{}
	byRoute map[uint64]map[uint64]struct{}
}

func newSessionRegistry() *sessionRegistry {
	return &sessionRegistry{
		items:   make(map[uint64]gatewaySessionRecord),
		byConn:  make(map[uint64]map[uint64]struct{}),
		byRoute: make(map[uint64]map[uint64]struct{}),
	}
}

func (r *sessionRegistry) Open(runtime gatewaySessionRuntime, snapshot gatewaySessionSnapshot) uint64 {
	id := gatewaySessionBaseID + r.nextID.Add(1)
	if id == 0 {
		id = gatewaySessionBaseID + r.nextID.Add(1)
	}

	item := gatewaySessionRecord{
		ID:       id,
		Runtime:  runtime,
		Snapshot: snapshot,
	}

	r.mu.Lock()
	r.items[id] = item
	if _, ok := r.byConn[runtime.ClientConnectionID]; !ok {
		r.byConn[runtime.ClientConnectionID] = make(map[uint64]struct{})
	}
	r.byConn[runtime.ClientConnectionID][id] = struct{}{}
	if _, ok := r.byRoute[runtime.RouteConnectionID]; !ok {
		r.byRoute[runtime.RouteConnectionID] = make(map[uint64]struct{})
	}
	r.byRoute[runtime.RouteConnectionID][id] = struct{}{}
	r.mu.Unlock()
	return id
}

func (r *sessionRegistry) Lookup(id uint64) (gatewaySessionRecord, bool) {
	r.mu.RLock()
	item, ok := r.items[id]
	r.mu.RUnlock()
	return item, ok
}

func (r *sessionRegistry) LookupOwned(id uint64, clientConnectionID uint64) (gatewaySessionRecord, bool) {
	item, ok := r.Lookup(id)
	if !ok || item.Runtime.ClientConnectionID != clientConnectionID {
		return gatewaySessionRecord{}, false
	}
	return item, true
}

func (r *sessionRegistry) Close(id uint64) (gatewaySessionRecord, bool) {
	r.mu.Lock()
	item, ok := r.items[id]
	if ok {
		delete(r.items, id)
		r.removeClientIndex(item.Runtime.ClientConnectionID, id)
		r.removeRouteIndex(item.Runtime.RouteConnectionID, id)
	}
	r.mu.Unlock()
	return item, ok
}

func (r *sessionRegistry) CloseConnection(connectionID uint64) []gatewaySessionRecord {
	r.mu.Lock()
	ids := r.collectClientSessionIDs(connectionID)
	items := make([]gatewaySessionRecord, 0, len(ids))
	for _, id := range ids {
		item, ok := r.items[id]
		if !ok {
			continue
		}
		items = append(items, item)
		delete(r.items, id)
		r.removeRouteIndex(item.Runtime.RouteConnectionID, id)
	}
	delete(r.byConn, connectionID)
	r.mu.Unlock()
	return items
}

func (r *sessionRegistry) CloseRouteConnection(routeConnectionID uint64) []gatewaySessionRecord {
	r.mu.Lock()
	ids := r.collectRouteSessionIDs(routeConnectionID)
	items := make([]gatewaySessionRecord, 0, len(ids))
	for _, id := range ids {
		item, ok := r.items[id]
		if !ok {
			continue
		}
		items = append(items, item)
		delete(r.items, id)
		r.removeClientIndex(item.Runtime.ClientConnectionID, id)
	}
	delete(r.byRoute, routeConnectionID)
	r.mu.Unlock()
	return items
}

func (r *sessionRegistry) collectClientSessionIDs(connectionID uint64) []uint64 {
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

func (r *sessionRegistry) removeClientIndex(connectionID uint64, sessionID uint64) {
	owned, ok := r.byConn[connectionID]
	if !ok {
		return
	}
	delete(owned, sessionID)
	if len(owned) == 0 {
		delete(r.byConn, connectionID)
	}
}

func (r *sessionRegistry) collectRouteSessionIDs(routeConnectionID uint64) []uint64 {
	owned := r.byRoute[routeConnectionID]
	if len(owned) == 0 {
		return nil
	}

	ids := make([]uint64, 0, len(owned))
	for id := range owned {
		ids = append(ids, id)
	}
	return ids
}

func (r *sessionRegistry) removeRouteIndex(routeConnectionID uint64, sessionID uint64) {
	owned, ok := r.byRoute[routeConnectionID]
	if !ok {
		return
	}
	delete(owned, sessionID)
	if len(owned) == 0 {
		delete(r.byRoute, routeConnectionID)
	}
}
