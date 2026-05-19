package storer

import (
	"net"
	"sync"
)

type connectionRegistry struct {
	mu          sync.RWMutex
	connections map[uint64]*connection
}

func newConnectionRegistry() *connectionRegistry {
	return &connectionRegistry{
		connections: make(map[uint64]*connection),
	}
}

func (r *connectionRegistry) Attach(connectionID uint64, conn net.Conn) *connection {
	connection := newConnection(connectionID, conn)
	r.mu.Lock()
	r.connections[connectionID] = connection
	r.mu.Unlock()
	return connection
}

func (r *connectionRegistry) Lookup(connectionID uint64) (*connection, bool) {
	r.mu.RLock()
	connection, ok := r.connections[connectionID]
	r.mu.RUnlock()
	return connection, ok
}

func (r *connectionRegistry) Remove(connectionID uint64) *connection {
	r.mu.Lock()
	connection := r.connections[connectionID]
	delete(r.connections, connectionID)
	r.mu.Unlock()
	return connection
}
