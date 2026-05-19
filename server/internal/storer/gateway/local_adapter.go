package gateway

import (
	"fmt"

	"yumedisk/server/internal/route"
	"yumedisk/server/internal/session"
)

const WholeRouteConnectionID uint64 = 1

type LocalCore interface {
	SessionService() *session.Service
	RouteEntry(routeTarget string, connectionID uint64) route.Entry
}

type LocalAdapter struct {
	core   LocalCore
	routes *route.Registry
}

func NewLocalAdapter(core LocalCore) (*LocalAdapter, error) {
	if core == nil {
		return nil, fmt.Errorf("local adapter requires core")
	}

	routes := route.NewRegistry()
	if err := routes.Register(core.RouteEntry("embedded://whole", WholeRouteConnectionID)); err != nil {
		return nil, err
	}
	return &LocalAdapter{
		core:   core,
		routes: routes,
	}, nil
}

func (b *LocalAdapter) LookupRoute(diskID string) (route.Entry, bool) {
	return b.routes.LookupRoute(diskID)
}

func (b *LocalAdapter) Open(connectionID uint64, entry route.Entry) (uint64, error) {
	desc, err := b.core.SessionService().Open(connectionID)
	if err != nil {
		return 0, err
	}
	return desc.ID, nil
}

func (b *LocalAdapter) Close(routeConnectionID uint64, sessionID uint64) {
	b.core.SessionService().Close(sessionID)
}

func (b *LocalAdapter) CloseConnection(connectionID uint64) {
	b.core.SessionService().CloseConnection(connectionID)
}

func (b *LocalAdapter) Read(routeConnectionID uint64, sessionID uint64, offset uint64, length uint32) ([]byte, error) {
	return b.core.SessionService().Read(sessionID, offset, length)
}

func (b *LocalAdapter) Write(routeConnectionID uint64, sessionID uint64, offset uint64, data []byte) error {
	return b.core.SessionService().Write(sessionID, offset, data)
}
