package gateway

import (
	"fmt"

	"yumedisk/server/internal/route"
	"yumedisk/server/internal/session"
)

type LocalExport interface {
	DiskID() string
	ReadOnly() bool
	SessionService() *session.Service
	RouteEntry(routeTarget string, connectionID uint64) route.Entry
}

type RouteSessionDataChangedHandler interface {
	NotifyRouteSessionDataChanged(routeConnectionID uint64, upstreamSessionID uint64)
}

type LocalAdapter struct {
	exportsByRoute      map[uint64]LocalExport
	routes              *route.Registry
	roRouteConnectionID uint64
	dataChangedHandler  RouteSessionDataChangedHandler
}

func NewLocalAdapter(exports []LocalExport) (*LocalAdapter, error) {
	if len(exports) == 0 {
		return nil, fmt.Errorf("local adapter requires at least one export")
	}

	routes := route.NewRegistry()
	exportsByRoute := make(map[uint64]LocalExport, len(exports))
	var roRouteConnectionID uint64
	for index, export := range exports {
		if export == nil {
			return nil, fmt.Errorf("local adapter export %d must not be nil", index)
		}
		routeConnectionID := uint64(index + 1)
		entry := export.RouteEntry("embedded://whole/"+export.DiskID(), routeConnectionID)
		if err := routes.Register(entry); err != nil {
			return nil, err
		}
		exportsByRoute[routeConnectionID] = export
		if export.ReadOnly() {
			roRouteConnectionID = routeConnectionID
		}
	}

	return &LocalAdapter{
		exportsByRoute:      exportsByRoute,
		routes:              routes,
		roRouteConnectionID: roRouteConnectionID,
	}, nil
}

func (b *LocalAdapter) LookupRoute(diskID string) (route.Entry, bool) {
	return b.routes.LookupRoute(diskID)
}

func (b *LocalAdapter) SetDataChangedHandler(handler RouteSessionDataChangedHandler) {
	b.dataChangedHandler = handler
}

func (b *LocalAdapter) NotifySessionDataChanged(sessionID uint64) {
	if b == nil || b.roRouteConnectionID == 0 || b.dataChangedHandler == nil {
		return
	}
	b.dataChangedHandler.NotifyRouteSessionDataChanged(b.roRouteConnectionID, sessionID)
}

func (b *LocalAdapter) Open(connectionID uint64, entry route.Entry) (uint64, error) {
	export, ok := b.exportsByRoute[entry.ConnectionID]
	if !ok {
		return 0, session.ErrSessionUnavailable
	}
	desc, err := export.SessionService().Open(connectionID)
	if err != nil {
		return 0, err
	}
	return desc.ID, nil
}

func (b *LocalAdapter) Describe(routeConnectionID uint64, sessionID uint64) (session.Metadata, error) {
	export, ok := b.exportsByRoute[routeConnectionID]
	if !ok {
		return session.Metadata{}, session.ErrSessionUnavailable
	}
	return export.SessionService().Describe(sessionID)
}

func (b *LocalAdapter) Close(routeConnectionID uint64, sessionID uint64) {
	export, ok := b.exportsByRoute[routeConnectionID]
	if !ok {
		return
	}
	export.SessionService().Close(sessionID)
}

func (b *LocalAdapter) CloseConnection(connectionID uint64) {
	for _, export := range b.exportsByRoute {
		export.SessionService().CloseConnection(connectionID)
	}
}

func (b *LocalAdapter) Read(routeConnectionID uint64, sessionID uint64, offset uint64, length uint32) ([]byte, error) {
	export, ok := b.exportsByRoute[routeConnectionID]
	if !ok {
		return nil, session.ErrSessionUnavailable
	}
	return export.SessionService().Read(sessionID, offset, length)
}

func (b *LocalAdapter) Write(routeConnectionID uint64, sessionID uint64, offset uint64, data []byte) error {
	export, ok := b.exportsByRoute[routeConnectionID]
	if !ok {
		return session.ErrSessionUnavailable
	}
	return export.SessionService().Write(sessionID, offset, data)
}
