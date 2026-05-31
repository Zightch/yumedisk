package gateway

import (
	"fmt"

	"yumedisk/server/internal/proto"
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

func (b *LocalAdapter) RoundTrip(routeConnectionID uint64, sessionID uint64, opCode uint8, body []byte) (uint16, []byte, error) {
	export, ok := b.exportsByRoute[routeConnectionID]
	if !ok {
		return proto.StatusSessionUnavailable, nil, nil
	}

	switch opCode {
	case proto.OpSessionDescribe:
		if sessionID == 0 {
			return proto.StatusBadHeader, nil, nil
		}
		if len(body) != 0 {
			return proto.StatusBadBody, nil, nil
		}
		metadata, err := export.SessionService().Describe(sessionID)
		if err != nil {
			return mapSessionErrorStatus(err), nil, nil
		}
		return proto.StatusOK, proto.BuildSessionDescribeResponseBody(
			metadata.DiskSizeBytes,
			metadata.MaxIOBytes,
			metadata.ReadOnly,
			metadata.BackendID,
		), nil
	case proto.OpReadAt:
		if sessionID == 0 {
			return proto.StatusBadHeader, nil, nil
		}
		offset, length, err := proto.ParseReadBody(body)
		if err != nil {
			return proto.StatusBadBody, nil, nil
		}
		data, err := export.SessionService().Read(sessionID, offset, length)
		if err != nil {
			return mapSessionErrorStatus(err), nil, nil
		}
		return proto.StatusOK, data, nil
	case proto.OpWriteAt:
		if sessionID == 0 {
			return proto.StatusBadHeader, nil, nil
		}
		offset, _, data, err := proto.ParseReadWriteBody(body)
		if err != nil {
			return proto.StatusBadBody, nil, nil
		}
		if err := export.SessionService().Write(sessionID, offset, data); err != nil {
			return mapSessionErrorStatus(err), nil, nil
		}
		return proto.StatusOK, nil, nil
	default:
		return proto.StatusUnsupportedOp, nil, nil
	}
}

func (b *LocalAdapter) SendNotice(routeConnectionID uint64, sessionID uint64, opCode uint8, body []byte) error {
	export, ok := b.exportsByRoute[routeConnectionID]
	if !ok {
		return nil
	}

	switch opCode {
	case proto.OpSessionCloseNotice:
		if sessionID == 0 {
			return fmt.Errorf("session close notice requires session id")
		}
		if _, err := proto.ParseSessionCloseNoticeBody(body); err != nil {
			return fmt.Errorf("session close notice body: %w", err)
		}
		export.SessionService().Close(sessionID)
		return nil
	default:
		return fmt.Errorf("unsupported local adapter notice op: %d", opCode)
	}
}

func (b *LocalAdapter) CloseConnection(connectionID uint64) {
	for _, export := range b.exportsByRoute {
		export.SessionService().CloseConnection(connectionID)
	}
}

func mapSessionErrorStatus(err error) uint16 {
	switch err {
	case session.ErrSessionUnavailable:
		return proto.StatusSessionUnavailable
	case session.ErrSessionOpenRejected:
		return proto.StatusSessionOpenRejected
	case session.ErrReadOnly:
		return proto.StatusIOReadOnly
	case session.ErrIOLimit:
		return proto.StatusIOLarge
	case session.ErrOutOfRange:
		return proto.StatusIOOutOfRange
	case session.ErrIOFailed:
		return proto.StatusIOFailed
	default:
		return proto.StatusIOFailed
	}
}
