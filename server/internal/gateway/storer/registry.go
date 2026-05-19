package storer

import (
	"sync"

	"yumedisk/server/internal/route"
)

type DisconnectHandler interface {
	CloseRouteConnection(routeConnectionID uint64, diskIDs []string)
}

type Registry struct {
	routes *route.Registry

	handlerMu sync.RWMutex
	handler   DisconnectHandler

	connections *connectionRegistry
}

func NewRegistry() *Registry {
	return &Registry{
		routes:      route.NewRegistry(),
		connections: newConnectionRegistry(),
	}
}

func (r *Registry) SetDisconnectHandler(handler DisconnectHandler) {
	r.handlerMu.Lock()
	r.handler = handler
	r.handlerMu.Unlock()
}

func (r *Registry) LookupRoute(diskID string) (route.Entry, bool) {
	return r.routes.LookupRoute(diskID)
}

func (r *Registry) Register(entry route.Entry) error {
	return r.routes.Register(entry)
}

func (r *Registry) DisconnectConnection(connectionID uint64) {
	disconnected := r.routes.DisconnectConnection(connectionID)
	conn := r.connections.Remove(connectionID)
	if conn != nil {
		conn.closePending()
	}

	r.handlerMu.RLock()
	handler := r.handler
	r.handlerMu.RUnlock()
	if handler == nil {
		return
	}

	diskIDs := make([]string, 0, len(disconnected))
	for _, entry := range disconnected {
		diskIDs = append(diskIDs, entry.DiskID)
	}
	handler.CloseRouteConnection(connectionID, diskIDs)
}
