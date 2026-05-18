package gateway

import (
	"net"
	"sync"
)

type storerConnectionRegistry struct {
	mu          sync.RWMutex
	connections map[uint64]*storerConnection
}

func newStorerConnectionRegistry() *storerConnectionRegistry {
	return &storerConnectionRegistry{
		connections: make(map[uint64]*storerConnection),
	}
}

func (r *storerConnectionRegistry) Attach(connectionID uint64, conn net.Conn) *storerConnection {
	connection := newStorerConnection(connectionID, conn)
	r.mu.Lock()
	r.connections[connectionID] = connection
	r.mu.Unlock()
	return connection
}

func (r *storerConnectionRegistry) Lookup(connectionID uint64) (*storerConnection, bool) {
	r.mu.RLock()
	connection, ok := r.connections[connectionID]
	r.mu.RUnlock()
	return connection, ok
}

func (r *storerConnectionRegistry) Remove(connectionID uint64) *storerConnection {
	r.mu.Lock()
	connection := r.connections[connectionID]
	delete(r.connections, connectionID)
	r.mu.Unlock()
	return connection
}
