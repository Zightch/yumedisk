package storer

import (
	"bytes"
	"net"
	"sync"

	"yumedisk/server/internal/route"
)

type DisconnectHandler interface {
	CloseRouteConnection(routeConnectionID uint64, diskIDs []string)
}

type CloseHandler interface {
	NotifyRouteSessionClosed(routeConnectionID uint64, upstreamSessionID uint64, body []byte)
}

type DataChangedHandler interface {
	NotifyRouteSessionDataChanged(routeConnectionID uint64, upstreamSessionID uint64)
}

type Registry struct {
	routes *route.Registry

	links              *activeLinks
	closeMu            sync.RWMutex
	closeHandler       CloseHandler
	dataChangedMu      sync.RWMutex
	dataChangedHandler DataChangedHandler
}

func NewRegistry() *Registry {
	return &Registry{
		routes: route.NewRegistry(),
		links:  newActiveLinks(),
	}
}

func (r *Registry) SetDisconnectHandler(handler DisconnectHandler) {
	r.links.SetDisconnectHandler(handler)
}

func (r *Registry) SetCloseHandler(handler CloseHandler) {
	r.closeMu.Lock()
	r.closeHandler = handler
	r.closeMu.Unlock()
}

func (r *Registry) SetDataChangedHandler(handler DataChangedHandler) {
	r.dataChangedMu.Lock()
	r.dataChangedHandler = handler
	r.dataChangedMu.Unlock()
}

func (r *Registry) LookupRoute(diskID string) (route.Entry, bool) {
	return r.routes.LookupRoute(diskID)
}

func (r *Registry) Register(entry route.Entry) error {
	return r.routes.Register(entry)
}

func (r *Registry) AttachConnection(connectionID uint64, conn net.Conn) *connection {
	return r.links.Attach(connectionID, conn)
}

func (r *Registry) DisconnectConnection(connectionID uint64) {
	disconnected := r.routes.DisconnectConnection(connectionID)
	r.links.Disconnect(connectionID, collectDiskIDs(disconnected))
}

func (r *Registry) NotifyRouteSessionClosed(routeConnectionID uint64, upstreamSessionID uint64, body []byte) {
	r.closeMu.RLock()
	handler := r.closeHandler
	r.closeMu.RUnlock()
	if handler == nil {
		return
	}
	handler.NotifyRouteSessionClosed(routeConnectionID, upstreamSessionID, bytes.Clone(body))
}

func (r *Registry) NotifyRouteSessionDataChanged(routeConnectionID uint64, upstreamSessionID uint64) {
	r.dataChangedMu.RLock()
	handler := r.dataChangedHandler
	r.dataChangedMu.RUnlock()
	if handler == nil {
		return
	}
	handler.NotifyRouteSessionDataChanged(routeConnectionID, upstreamSessionID)
}

func collectDiskIDs(entries []route.Entry) []string {
	if len(entries) == 0 {
		return nil
	}

	diskIDs := make([]string, 0, len(entries))
	for _, entry := range entries {
		diskIDs = append(diskIDs, entry.DiskID)
	}
	return diskIDs
}
