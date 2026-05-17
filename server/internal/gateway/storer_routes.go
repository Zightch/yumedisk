package gateway

import "yumedisk/server/internal/route"

type StorerRouteRegistry struct {
	registry *route.Registry
}

func NewStorerRouteRegistry() *StorerRouteRegistry {
	return &StorerRouteRegistry{
		registry: route.NewRegistry(),
	}
}

func (r *StorerRouteRegistry) LookupRoute(diskID string) (route.Entry, bool) {
	return r.registry.LookupRoute(diskID)
}

func (r *StorerRouteRegistry) Register(entry route.Entry) error {
	return r.registry.Register(entry)
}

func (r *StorerRouteRegistry) DisconnectConnection(connectionID uint64) {
	r.registry.DisconnectConnection(connectionID)
}
