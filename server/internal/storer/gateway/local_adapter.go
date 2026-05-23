package gateway

import (
	"fmt"

	"yumedisk/server/internal/route"
	"yumedisk/server/internal/session"
)

const WholeRouteConnectionID uint64 = 1

type LocalExport interface {
	DiskID() string
	SessionService() *session.Service
	RouteEntry(routeTarget string, connectionID uint64) route.Entry
}

type LocalAdapter struct {
	export LocalExport
	routes *route.Registry
}

func NewLocalAdapter(export LocalExport) (*LocalAdapter, error) {
	if export == nil {
		return nil, fmt.Errorf("local adapter requires export")
	}

	routes := route.NewRegistry()
	if err := routes.Register(export.RouteEntry("embedded://whole", WholeRouteConnectionID)); err != nil {
		return nil, err
	}
	return &LocalAdapter{
		export: export,
		routes: routes,
	}, nil
}

func (b *LocalAdapter) LookupRoute(diskID string) (route.Entry, bool) {
	return b.routes.LookupRoute(diskID)
}

func (b *LocalAdapter) Open(connectionID uint64, entry route.Entry) (uint64, error) {
	desc, err := b.export.SessionService().Open(connectionID)
	if err != nil {
		return 0, err
	}
	return desc.ID, nil
}

func (b *LocalAdapter) Close(routeConnectionID uint64, sessionID uint64) {
	b.export.SessionService().Close(sessionID)
}

func (b *LocalAdapter) CloseConnection(connectionID uint64) {
	b.export.SessionService().CloseConnection(connectionID)
}

func (b *LocalAdapter) Read(routeConnectionID uint64, sessionID uint64, offset uint64, length uint32) ([]byte, error) {
	return b.export.SessionService().Read(sessionID, offset, length)
}

func (b *LocalAdapter) Write(routeConnectionID uint64, sessionID uint64, offset uint64, data []byte) error {
	return b.export.SessionService().Write(sessionID, offset, data)
}
