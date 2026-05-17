package gateway

import (
	"sync"
	"sync/atomic"
)

const gatewaySessionBaseID = uint64(1) << 63

type gatewaySession struct {
	ID               uint64
	ClientConnection uint64
	RouteConnection  uint64
	UpstreamSession  uint64
}

type sessionRegistry struct {
	nextID atomic.Uint64

	mu     sync.RWMutex
	items  map[uint64]gatewaySession
	byConn map[uint64]map[uint64]struct{}
}

func newSessionRegistry() *sessionRegistry {
	return &sessionRegistry{
		items:  make(map[uint64]gatewaySession),
		byConn: make(map[uint64]map[uint64]struct{}),
	}
}

func (r *sessionRegistry) Open(clientConnectionID uint64, routeConnectionID uint64, upstreamSessionID uint64) uint64 {
	id := gatewaySessionBaseID + r.nextID.Add(1)
	if id == 0 {
		id = gatewaySessionBaseID + r.nextID.Add(1)
	}

	item := gatewaySession{
		ID:               id,
		ClientConnection: clientConnectionID,
		RouteConnection:  routeConnectionID,
		UpstreamSession:  upstreamSessionID,
	}

	r.mu.Lock()
	r.items[id] = item
	if _, ok := r.byConn[clientConnectionID]; !ok {
		r.byConn[clientConnectionID] = make(map[uint64]struct{})
	}
	r.byConn[clientConnectionID][id] = struct{}{}
	r.mu.Unlock()
	return id
}

func (r *sessionRegistry) Lookup(id uint64) (gatewaySession, bool) {
	r.mu.RLock()
	item, ok := r.items[id]
	r.mu.RUnlock()
	return item, ok
}

func (r *sessionRegistry) Close(id uint64) (gatewaySession, bool) {
	r.mu.Lock()
	item, ok := r.items[id]
	if ok {
		delete(r.items, id)
		r.removeClientIndex(item.ClientConnection, id)
	}
	r.mu.Unlock()
	return item, ok
}

func (r *sessionRegistry) CloseConnection(connectionID uint64) []gatewaySession {
	r.mu.Lock()
	ids := r.collectClientSessionIDs(connectionID)
	items := make([]gatewaySession, 0, len(ids))
	for _, id := range ids {
		item, ok := r.items[id]
		if !ok {
			continue
		}
		items = append(items, item)
		delete(r.items, id)
	}
	delete(r.byConn, connectionID)
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
