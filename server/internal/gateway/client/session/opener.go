package session

import (
	"fmt"

	"yumedisk/server/internal/proto"
	serversession "yumedisk/server/internal/session"
)

type Opener struct {
	routes   RouteSource
	sessions DataPlane
	grants   GrantRegistry
	registry *registry
}

func NewOpener(routes RouteSource, sessions DataPlane, grants GrantRegistry) *Opener {
	return &Opener{
		routes:   routes,
		sessions: sessions,
		grants:   grants,
		registry: newRegistry(),
	}
}

func (o *Opener) HandleSessionOpen(state ConnectionState, header proto.Header, body []byte) ([]byte, error) {
	if header.SessionID != 0 {
		return proto.BuildErrorResponse(header, proto.StatusBadHeader), nil
	}

	authID, err := proto.ParseSessionOpenRequestBody(body)
	if err != nil {
		return proto.BuildErrorResponse(header, proto.StatusBadBody), nil
	}
	if err := state.BeginSessionOpen(); err != nil {
		return proto.BuildErrorResponse(header, proto.StatusInvalidRequest), nil
	}

	success := false
	defer func() {
		if !success {
			state.FailSessionOpen()
		}
	}()

	diskID, status, ok := o.grants.LookupDisk(authID, state.ConnectionID())
	if !ok {
		return proto.BuildErrorResponse(header, status), nil
	}

	routeEntry, ok := o.routes.LookupRoute(diskID)
	if !ok {
		return proto.BuildErrorResponse(header, proto.StatusSessionUnavailable), nil
	}

	upstreamSessionID, err := o.sessions.Open(state.ConnectionID(), routeEntry)
	if err != nil {
		return o.mapSessionError(header, err), nil
	}

	gatewaySessionID := o.registry.Open(Record{
		ClientConnectionID: state.ConnectionID(),
		RouteConnectionID:  routeEntry.ConnectionID,
		UpstreamSessionID:  upstreamSessionID,
		DiskID:             routeEntry.DiskID,
		DiskSizeBytes:      routeEntry.DiskSizeBytes,
		ReadOnly:           routeEntry.ReadOnly,
		MaxIOBytes:         routeEntry.MaxIOBytes,
	})
	if consumedDiskID, ok := o.grants.ConsumeDisk(authID); !ok || consumedDiskID != routeEntry.DiskID {
		o.registry.Close(gatewaySessionID)
		o.sessions.Close(routeEntry.ConnectionID, upstreamSessionID)
		return proto.BuildErrorResponse(header, proto.StatusAuthIDInvalid), nil
	}
	if err := state.FinishSessionOpen(); err != nil {
		o.registry.Close(gatewaySessionID)
		o.sessions.Close(routeEntry.ConnectionID, upstreamSessionID)
		return proto.BuildErrorResponse(header, proto.StatusInvalidRequest), nil
	}

	success = true
	return proto.BuildResponseWithSessionID(header, proto.StatusOK, gatewaySessionID, nil), nil
}

func (o *Opener) HandleDescribe(state ConnectionState, header proto.Header, body []byte) ([]byte, error) {
	if header.SessionID == 0 {
		return proto.BuildErrorResponse(header, proto.StatusBadHeader), nil
	}
	if len(body) != 0 {
		return proto.BuildErrorResponse(header, proto.StatusBadBody), nil
	}

	record, ok := o.registry.LookupOwned(header.SessionID, state.ConnectionID())
	if !ok {
		return proto.BuildErrorResponse(header, proto.StatusSessionUnavailable), nil
	}

	bodyOut := proto.BuildSessionDescribeResponseBody(record.DiskSizeBytes, record.MaxIOBytes, record.ReadOnly)
	return proto.BuildSuccessResponse(header, bodyOut), nil
}

func (o *Opener) HandleConnHeartbeat(state ConnectionState, header proto.Header, body []byte) ([]byte, error) {
	if header.SessionID != 0 {
		return proto.BuildErrorResponse(header, proto.StatusBadHeader), nil
	}
	if len(body) != 0 {
		return proto.BuildErrorResponse(header, proto.StatusBadBody), nil
	}
	state.MarkHeartbeat()
	return proto.BuildSuccessResponse(header, nil), nil
}

func (o *Opener) HandleSessionCloseNotice(state ConnectionState, header proto.Header, body []byte) ([]byte, error) {
	if header.SessionID == 0 {
		return nil, fmt.Errorf("session close notice requires session id")
	}
	if _, err := proto.ParseSessionCloseNoticeBody(body); err != nil {
		return nil, fmt.Errorf("session close notice body: %w", err)
	}

	record, ok := o.registry.LookupOwned(header.SessionID, state.ConnectionID())
	if !ok {
		return nil, nil
	}

	o.registry.Close(header.SessionID)
	o.sessions.Close(record.RouteConnectionID, record.UpstreamSessionID)
	return nil, nil
}

func (o *Opener) HandleRead(state ConnectionState, header proto.Header, body []byte) ([]byte, error) {
	if header.SessionID == 0 {
		return proto.BuildErrorResponse(header, proto.StatusBadHeader), nil
	}

	offset, length, err := proto.ParseReadBody(body)
	if err != nil {
		return proto.BuildErrorResponse(header, proto.StatusBadBody), nil
	}

	record, ok := o.registry.LookupOwned(header.SessionID, state.ConnectionID())
	if !ok {
		return proto.BuildErrorResponse(header, proto.StatusSessionUnavailable), nil
	}

	data, err := o.sessions.Read(record.RouteConnectionID, record.UpstreamSessionID, offset, length)
	if err != nil {
		if err == serversession.ErrSessionUnavailable {
			o.registry.Close(header.SessionID)
		}
		return o.mapSessionError(header, err), nil
	}
	return proto.BuildSuccessResponse(header, data), nil
}

func (o *Opener) HandleWrite(state ConnectionState, header proto.Header, body []byte) ([]byte, error) {
	if header.SessionID == 0 {
		return proto.BuildErrorResponse(header, proto.StatusBadHeader), nil
	}

	offset, _, data, err := proto.ParseReadWriteBody(body)
	if err != nil {
		return proto.BuildErrorResponse(header, proto.StatusBadBody), nil
	}

	record, ok := o.registry.LookupOwned(header.SessionID, state.ConnectionID())
	if !ok {
		return proto.BuildErrorResponse(header, proto.StatusSessionUnavailable), nil
	}

	if err := o.sessions.Write(record.RouteConnectionID, record.UpstreamSessionID, offset, data); err != nil {
		if err == serversession.ErrSessionUnavailable {
			o.registry.Close(header.SessionID)
		}
		return o.mapSessionError(header, err), nil
	}
	return proto.BuildSuccessResponse(header, nil), nil
}

func (o *Opener) CloseConnection(connectionID uint64) {
	for _, record := range o.registry.CloseConnection(connectionID) {
		o.sessions.Close(record.RouteConnectionID, record.UpstreamSessionID)
	}
	o.sessions.CloseConnection(connectionID)
}

func (o *Opener) CloseRouteConnection(routeConnectionID uint64) []Record {
	return o.registry.CloseRouteConnection(routeConnectionID)
}

func (o *Opener) mapSessionError(header proto.Header, err error) []byte {
	switch err {
	case serversession.ErrSessionUnavailable:
		return proto.BuildErrorResponse(header, proto.StatusSessionUnavailable)
	case serversession.ErrSessionOpenRejected:
		return proto.BuildErrorResponse(header, proto.StatusSessionOpenRejected)
	case serversession.ErrReadOnly:
		return proto.BuildErrorResponse(header, proto.StatusIOReadOnly)
	case serversession.ErrIOLimit:
		return proto.BuildErrorResponse(header, proto.StatusIOLarge)
	case serversession.ErrOutOfRange:
		return proto.BuildErrorResponse(header, proto.StatusIOOutOfRange)
	case serversession.ErrIOFailed:
		return proto.BuildErrorResponse(header, proto.StatusIOFailed)
	default:
		return proto.BuildErrorResponse(header, proto.StatusIOFailed)
	}
}
