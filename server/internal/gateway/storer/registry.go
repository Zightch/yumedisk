package storer

import (
	"net"

	"yumedisk/server/internal/route"
)

type DisconnectHandler interface {
	CloseRouteConnection(routeConnectionID uint64, diskIDs []string)
}

type Registry struct {
	routes *route.Registry

	links *activeLinks
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
