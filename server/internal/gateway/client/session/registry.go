package session

import (
	"sync"
	"sync/atomic"
)

const gatewaySessionBaseID = uint64(1) << 63

type Record struct {
	ID                 uint64
	ClientConnectionID uint64
	RouteConnectionID  uint64
	UpstreamSessionID  uint64
	DiskID             string
	DiskSizeBytes      uint64
	ReadOnly           bool
	MaxIOBytes         uint32
}

type registry struct {
	nextID atomic.Uint64

	mu      sync.RWMutex
	items   map[uint64]Record
	byConn  map[uint64]map[uint64]struct{}
	byRoute map[uint64]map[uint64]struct{}
}

func newRegistry() *registry {
	return &registry{
		items:   make(map[uint64]Record),
		byConn:  make(map[uint64]map[uint64]struct{}),
		byRoute: make(map[uint64]map[uint64]struct{}),
	}
}

func (r *registry) Open(record Record) uint64 {
	id := gatewaySessionBaseID + r.nextID.Add(1)
	if id == 0 {
		id = gatewaySessionBaseID + r.nextID.Add(1)
	}
	record.ID = id

	r.mu.Lock()
	r.items[id] = record
	if _, ok := r.byConn[record.ClientConnectionID]; !ok {
		r.byConn[record.ClientConnectionID] = make(map[uint64]struct{})
	}
	r.byConn[record.ClientConnectionID][id] = struct{}{}
	if _, ok := r.byRoute[record.RouteConnectionID]; !ok {
		r.byRoute[record.RouteConnectionID] = make(map[uint64]struct{})
	}
	r.byRoute[record.RouteConnectionID][id] = struct{}{}
	r.mu.Unlock()
	return id
}

func (r *registry) Lookup(id uint64) (Record, bool) {
	r.mu.RLock()
	record, ok := r.items[id]
	r.mu.RUnlock()
	return record, ok
}

func (r *registry) LookupOwned(id uint64, clientConnectionID uint64) (Record, bool) {
	record, ok := r.Lookup(id)
	if !ok || record.ClientConnectionID != clientConnectionID {
		return Record{}, false
	}
	return record, true
}

func (r *registry) Close(id uint64) (Record, bool) {
	r.mu.Lock()
	record, ok := r.items[id]
	if ok {
		delete(r.items, id)
		r.removeClientIndex(record.ClientConnectionID, id)
		r.removeRouteIndex(record.RouteConnectionID, id)
	}
	r.mu.Unlock()
	return record, ok
}

func (r *registry) CloseConnection(connectionID uint64) []Record {
	r.mu.Lock()
	ids := r.collectClientSessionIDs(connectionID)
	records := make([]Record, 0, len(ids))
	for _, id := range ids {
		record, ok := r.items[id]
		if !ok {
			continue
		}
		records = append(records, record)
		delete(r.items, id)
		r.removeRouteIndex(record.RouteConnectionID, id)
	}
	delete(r.byConn, connectionID)
	r.mu.Unlock()
	return records
}

func (r *registry) CloseRouteConnection(routeConnectionID uint64) []Record {
	r.mu.Lock()
	ids := r.collectRouteSessionIDs(routeConnectionID)
	records := make([]Record, 0, len(ids))
	for _, id := range ids {
		record, ok := r.items[id]
		if !ok {
			continue
		}
		records = append(records, record)
		delete(r.items, id)
		r.removeClientIndex(record.ClientConnectionID, id)
	}
	delete(r.byRoute, routeConnectionID)
	r.mu.Unlock()
	return records
}

func (r *registry) collectClientSessionIDs(connectionID uint64) []uint64 {
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

func (r *registry) removeClientIndex(connectionID uint64, sessionID uint64) {
	owned, ok := r.byConn[connectionID]
	if !ok {
		return
	}
	delete(owned, sessionID)
	if len(owned) == 0 {
		delete(r.byConn, connectionID)
	}
}

func (r *registry) collectRouteSessionIDs(routeConnectionID uint64) []uint64 {
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

func (r *registry) removeRouteIndex(routeConnectionID uint64, sessionID uint64) {
	owned, ok := r.byRoute[routeConnectionID]
	if !ok {
		return
	}
	delete(owned, sessionID)
	if len(owned) == 0 {
		delete(r.byRoute, routeConnectionID)
	}
}
