package gateway

import (
	"yumedisk/server/internal/proto"
	"yumedisk/server/internal/session"
)

type sessionOpener struct {
	routes   RouteSource
	sessions SessionDataPlane
	grants   *authGrantRegistry
	registry *sessionRegistry
}

func newSessionOpener(routes RouteSource, sessions SessionDataPlane, grants *authGrantRegistry) *sessionOpener {
	return &sessionOpener{
		routes:   routes,
		sessions: sessions,
		grants:   grants,
		registry: newSessionRegistry(),
	}
}

func (o *sessionOpener) handleSessionOpen(state *ConnectionState, header proto.Header, body []byte) ([]byte, error) {
	if header.SessionID != 0 {
		return proto.BuildErrorResponse(header, proto.StatusBadHeader), nil
	}

	authID, err := proto.ParseSessionOpenRequestBody(body)
	if err != nil {
		return proto.BuildErrorResponse(header, proto.StatusBadBody), nil
	}
	if err := state.beginSessionOpen(); err != nil {
		return proto.BuildErrorResponse(header, proto.StatusInvalidRequest), nil
	}

	success := false
	defer func() {
		if !success {
			state.failSessionOpen()
		}
	}()

	grant, status, ok := o.grants.Lookup(authID, state.ID)
	if !ok {
		return proto.BuildErrorResponse(header, status), nil
	}

	routeEntry, ok := o.routes.LookupRoute(grant.DiskID)
	if !ok {
		return proto.BuildErrorResponse(header, proto.StatusSessionUnavailable), nil
	}

	upstreamSessionID, err := o.sessions.Open(state.ID, routeEntry)
	if err != nil {
		return o.mapSessionError(header, err), nil
	}

	gatewaySessionID := o.registry.Open(
		gatewaySessionRuntime{
			ClientConnectionID: state.ID,
			RouteConnectionID:  routeEntry.ConnectionID,
			UpstreamSessionID:  upstreamSessionID,
		},
		gatewaySessionSnapshot{
			DiskID:        routeEntry.DiskID,
			DiskSizeBytes: routeEntry.DiskSizeBytes,
			ReadOnly:      routeEntry.ReadOnly,
			MaxIOBytes:    routeEntry.MaxIOBytes,
		},
	)
	if _, ok := o.grants.Consume(authID); !ok {
		o.registry.Close(gatewaySessionID)
		o.sessions.Close(routeEntry.ConnectionID, upstreamSessionID)
		return proto.BuildErrorResponse(header, proto.StatusAuthIDInvalid), nil
	}
	if err := state.finishSessionOpen(); err != nil {
		o.registry.Close(gatewaySessionID)
		o.sessions.Close(routeEntry.ConnectionID, upstreamSessionID)
		return proto.BuildErrorResponse(header, proto.StatusInvalidRequest), nil
	}

	success = true
	return proto.BuildResponseWithSessionID(header, proto.StatusOK, gatewaySessionID, nil), nil
}

func (o *sessionOpener) handleDescribe(state *ConnectionState, header proto.Header, body []byte) ([]byte, error) {
	if header.SessionID == 0 {
		return proto.BuildErrorResponse(header, proto.StatusBadHeader), nil
	}
	if len(body) != 0 {
		return proto.BuildErrorResponse(header, proto.StatusBadBody), nil
	}

	mapped, ok := o.registry.LookupOwned(header.SessionID, state.ID)
	if !ok {
		return proto.BuildErrorResponse(header, proto.StatusSessionUnavailable), nil
	}

	bodyOut := proto.BuildSessionDescribeResponseBody(mapped.Snapshot.DiskSizeBytes, mapped.Snapshot.MaxIOBytes, mapped.Snapshot.ReadOnly)
	return proto.BuildSuccessResponse(header, bodyOut), nil
}

func (o *sessionOpener) handleConnHeartbeat(header proto.Header, body []byte) ([]byte, error) {
	if header.SessionID != 0 {
		return proto.BuildErrorResponse(header, proto.StatusBadHeader), nil
	}
	if len(body) != 0 {
		return proto.BuildErrorResponse(header, proto.StatusBadBody), nil
	}
	return proto.BuildSuccessResponse(header, nil), nil
}

func (o *sessionOpener) handleClose(state *ConnectionState, header proto.Header, body []byte) ([]byte, error) {
	if header.SessionID == 0 {
		return proto.BuildErrorResponse(header, proto.StatusBadHeader), nil
	}
	if len(body) != 0 {
		return proto.BuildErrorResponse(header, proto.StatusBadBody), nil
	}

	mapped, ok := o.registry.LookupOwned(header.SessionID, state.ID)
	if !ok {
		return proto.BuildErrorResponse(header, proto.StatusSessionUnavailable), nil
	}

	o.registry.Close(header.SessionID)
	o.sessions.Close(mapped.Runtime.RouteConnectionID, mapped.Runtime.UpstreamSessionID)
	return proto.BuildSuccessResponse(header, nil), nil
}

func (o *sessionOpener) handleRead(state *ConnectionState, header proto.Header, body []byte) ([]byte, error) {
	if header.SessionID == 0 {
		return proto.BuildErrorResponse(header, proto.StatusBadHeader), nil
	}

	offset, length, err := proto.ParseReadBody(body)
	if err != nil {
		return proto.BuildErrorResponse(header, proto.StatusBadBody), nil
	}

	mapped, ok := o.registry.LookupOwned(header.SessionID, state.ID)
	if !ok {
		return proto.BuildErrorResponse(header, proto.StatusSessionUnavailable), nil
	}

	data, err := o.sessions.Read(mapped.Runtime.RouteConnectionID, mapped.Runtime.UpstreamSessionID, offset, length)
	if err != nil {
		if err == session.ErrSessionUnavailable {
			o.registry.Close(header.SessionID)
		}
		return o.mapSessionError(header, err), nil
	}
	return proto.BuildSuccessResponse(header, data), nil
}

func (o *sessionOpener) handleWrite(state *ConnectionState, header proto.Header, body []byte) ([]byte, error) {
	if header.SessionID == 0 {
		return proto.BuildErrorResponse(header, proto.StatusBadHeader), nil
	}

	offset, _, data, err := proto.ParseReadWriteBody(body)
	if err != nil {
		return proto.BuildErrorResponse(header, proto.StatusBadBody), nil
	}

	mapped, ok := o.registry.LookupOwned(header.SessionID, state.ID)
	if !ok {
		return proto.BuildErrorResponse(header, proto.StatusSessionUnavailable), nil
	}

	if err := o.sessions.Write(mapped.Runtime.RouteConnectionID, mapped.Runtime.UpstreamSessionID, offset, data); err != nil {
		if err == session.ErrSessionUnavailable {
			o.registry.Close(header.SessionID)
		}
		return o.mapSessionError(header, err), nil
	}
	return proto.BuildSuccessResponse(header, nil), nil
}

func (o *sessionOpener) closeConnection(connectionID uint64) {
	for _, mapped := range o.registry.CloseConnection(connectionID) {
		o.sessions.Close(mapped.Runtime.RouteConnectionID, mapped.Runtime.UpstreamSessionID)
	}
	o.sessions.CloseConnection(connectionID)
}

func (o *sessionOpener) closeRouteConnection(routeConnectionID uint64) []gatewaySessionRecord {
	return o.registry.CloseRouteConnection(routeConnectionID)
}

func (o *sessionOpener) mapSessionError(header proto.Header, err error) []byte {
	switch err {
	case session.ErrSessionUnavailable:
		return proto.BuildErrorResponse(header, proto.StatusSessionUnavailable)
	case session.ErrSessionOpenRejected:
		return proto.BuildErrorResponse(header, proto.StatusSessionOpenRejected)
	case session.ErrReadOnly:
		return proto.BuildErrorResponse(header, proto.StatusIOReadOnly)
	case session.ErrIOLimit:
		return proto.BuildErrorResponse(header, proto.StatusIOLarge)
	case session.ErrOutOfRange:
		return proto.BuildErrorResponse(header, proto.StatusIOOutOfRange)
	case session.ErrIOFailed:
		return proto.BuildErrorResponse(header, proto.StatusIOFailed)
	default:
		return proto.BuildErrorResponse(header, proto.StatusIOFailed)
	}
}
