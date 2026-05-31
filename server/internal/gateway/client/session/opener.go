package session

import (
	"fmt"

	connectionpkg "yumedisk/server/internal/gateway/client/connection"
	"yumedisk/server/internal/proto"
	serversession "yumedisk/server/internal/session"
)

type Opener struct {
	routes   RouteSource
	sessions RouteSessionProxy
	grants   GrantRegistry
	registry *registry
}

func NewOpener(routes RouteSource, sessions RouteSessionProxy, grants GrantRegistry) *Opener {
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
	})
	if consumedDiskID, ok := o.grants.ConsumeDisk(authID); !ok || consumedDiskID != routeEntry.DiskID {
		o.registry.Close(gatewaySessionID)
		o.closeUpstreamSession(routeEntry.ConnectionID, upstreamSessionID, proto.SessionCloseReasonNormalClose)
		return proto.BuildErrorResponse(header, proto.StatusAuthIDInvalid), nil
	}
	if err := state.FinishSessionOpen(); err != nil {
		o.registry.Close(gatewaySessionID)
		o.closeUpstreamSession(routeEntry.ConnectionID, upstreamSessionID, proto.SessionCloseReasonNormalClose)
		return proto.BuildErrorResponse(header, proto.StatusInvalidRequest), nil
	}

	success = true
	return proto.BuildResponseWithSessionID(header, proto.StatusOK, gatewaySessionID, nil), nil
}

func (o *Opener) HandleDescribe(state ConnectionState, header proto.Header, body []byte) ([]byte, error) {
	if header.SessionID == 0 {
		return proto.BuildErrorResponse(header, proto.StatusBadHeader), nil
	}

	record, ok := o.registry.LookupOwned(header.SessionID, state.ConnectionID())
	if !ok {
		return proto.BuildErrorResponse(header, proto.StatusSessionUnavailable), nil
	}
	return o.handleProxyRoundTrip(header, body, record), nil
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
		return nil, fmt.Errorf("%w: session close notice requires session id", connectionpkg.ErrProtocolViolation)
	}
	if _, err := proto.ParseSessionCloseNoticeBody(body); err != nil {
		return nil, fmt.Errorf("%w: session close notice body: %v", connectionpkg.ErrProtocolViolation, err)
	}

	record, ok := o.registry.LookupOwned(header.SessionID, state.ConnectionID())
	if !ok {
		return nil, nil
	}

	o.registry.Close(header.SessionID)
	if err := o.sessions.SendNotice(record.RouteConnectionID, record.UpstreamSessionID, header.OpCode, body); err != nil {
		return nil, fmt.Errorf("session close notice proxy: %w", err)
	}
	return nil, nil
}

func (o *Opener) HandleRead(state ConnectionState, header proto.Header, body []byte) ([]byte, error) {
	if header.SessionID == 0 {
		return proto.BuildErrorResponse(header, proto.StatusBadHeader), nil
	}

	record, ok := o.registry.LookupOwned(header.SessionID, state.ConnectionID())
	if !ok {
		return proto.BuildErrorResponse(header, proto.StatusSessionUnavailable), nil
	}
	return o.handleProxyRoundTrip(header, body, record), nil
}

func (o *Opener) HandleWrite(state ConnectionState, header proto.Header, body []byte) ([]byte, error) {
	if header.SessionID == 0 {
		return proto.BuildErrorResponse(header, proto.StatusBadHeader), nil
	}

	record, ok := o.registry.LookupOwned(header.SessionID, state.ConnectionID())
	if !ok {
		return proto.BuildErrorResponse(header, proto.StatusSessionUnavailable), nil
	}
	return o.handleProxyRoundTrip(header, body, record), nil
}

func (o *Opener) CloseConnection(connectionID uint64) {
	o.CloseConnectionWithReason(connectionID, proto.SessionCloseReasonNormalClose)
}

func (o *Opener) CloseConnectionWithReason(connectionID uint64, reason uint16) {
	for _, record := range o.registry.CloseConnection(connectionID) {
		o.closeUpstreamSession(record.RouteConnectionID, record.UpstreamSessionID, reason)
	}
	o.sessions.CloseConnection(connectionID)
}

func (o *Opener) CloseRouteConnection(routeConnectionID uint64) []Record {
	return o.registry.CloseRouteConnection(routeConnectionID)
}

func (o *Opener) LookupRouteSession(routeConnectionID uint64, upstreamSessionID uint64) (Record, bool) {
	return o.registry.LookupRouteSession(routeConnectionID, upstreamSessionID)
}

func (o *Opener) CloseRouteSession(routeConnectionID uint64, upstreamSessionID uint64) (Record, bool) {
	return o.registry.CloseRouteSession(routeConnectionID, upstreamSessionID)
}

func (o *Opener) handleProxyRoundTrip(header proto.Header, body []byte, record Record) []byte {
	status, responseBody, err := o.sessions.RoundTrip(
		record.RouteConnectionID,
		record.UpstreamSessionID,
		header.OpCode,
		body,
	)
	if err != nil {
		o.registry.Close(header.SessionID)
		return proto.BuildErrorResponse(header, proto.StatusSessionUnavailable)
	}
	if status == proto.StatusSessionUnavailable {
		o.registry.Close(header.SessionID)
	}
	return proto.BuildResponseWithSessionID(header, status, header.SessionID, responseBody)
}

func (o *Opener) closeUpstreamSession(routeConnectionID uint64, upstreamSessionID uint64, reason uint16) {
	_ = o.sessions.SendNotice(
		routeConnectionID,
		upstreamSessionID,
		proto.OpSessionCloseNotice,
		proto.BuildSessionCloseNoticeBody(reason),
	)
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
