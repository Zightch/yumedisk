package gateway

import (
	"yumedisk/server/internal/proto"
	"yumedisk/server/internal/session"
)

type sessionOpener struct {
	routes   RouteSource
	sessions SessionDataPlane
	registry *sessionRegistry
}

func newSessionOpener(routes RouteSource, sessions SessionDataPlane) *sessionOpener {
	return &sessionOpener{
		routes:   routes,
		sessions: sessions,
		registry: newSessionRegistry(),
	}
}

func (o *sessionOpener) handleSessionOpen(state *ConnectionState, header proto.Header, body []byte) ([]byte, error) {
	if header.SessionID != 0 {
		return proto.BuildErrorResponse(header, proto.StatusBadHeader), nil
	}

	diskID, err := proto.ParseSessionOpenRequestBody(body)
	if err != nil {
		return proto.BuildErrorResponse(header, proto.StatusBadBody), nil
	}
	if !state.isAuthenticated(diskID) {
		return proto.BuildErrorResponse(header, proto.StatusAuthRequired), nil
	}
	routeEntry, ok := o.routes.LookupRoute(diskID)
	if !ok {
		return proto.BuildErrorResponse(header, proto.StatusSessionUnavailable), nil
	}

	desc, err := o.sessions.Open(state.ID, diskID)
	if err != nil {
		return o.mapSessionError(header, err), nil
	}
	gatewaySessionID := o.registry.Open(state.ID, routeEntry.ConnectionID, desc.ID)
	bodyOut := proto.BuildSessionOpenResponseBody(desc.DiskSize, desc.MaxIOBytes, o.sessions.TTLSeconds(), desc.ReadOnly)
	respHeader := proto.Header{
		ProtocolVersion: header.ProtocolVersion,
		HeaderLen:       header.HeaderLen,
		OpCode:          header.OpCode,
		Flags:           proto.FlagResponse,
		StatusCode:      proto.StatusOK,
		Reserved:        0,
		RequestID:       header.RequestID,
		SessionID:       gatewaySessionID,
	}
	return proto.BuildResponse(respHeader, proto.StatusOK, bodyOut), nil
}

func (o *sessionOpener) handlePing(header proto.Header, body []byte) ([]byte, error) {
	if header.SessionID == 0 {
		return proto.BuildErrorResponse(header, proto.StatusBadHeader), nil
	}

	nonce, err := proto.ParsePingRequestBody(body)
	if err != nil {
		return proto.BuildErrorResponse(header, proto.StatusBadBody), nil
	}

	mapped, ok := o.registry.Lookup(header.SessionID)
	if !ok {
		return proto.BuildErrorResponse(header, proto.StatusSessionUnavailable), nil
	}

	_, ok = o.sessions.Ping(mapped.UpstreamSession)
	if !ok {
		o.registry.Close(header.SessionID)
		return proto.BuildErrorResponse(header, proto.StatusSessionUnavailable), nil
	}
	return proto.BuildSuccessResponse(header, proto.BuildPingResponseBody(nonce)), nil
}

func (o *sessionOpener) handleClose(header proto.Header, body []byte) ([]byte, error) {
	if header.SessionID == 0 {
		return proto.BuildErrorResponse(header, proto.StatusBadHeader), nil
	}
	if len(body) != 0 {
		return proto.BuildErrorResponse(header, proto.StatusBadBody), nil
	}

	mapped, ok := o.registry.Close(header.SessionID)
	if ok {
		o.sessions.Close(mapped.UpstreamSession)
	}
	return proto.BuildSuccessResponse(header, nil), nil
}

func (o *sessionOpener) handleRead(header proto.Header, body []byte) ([]byte, error) {
	if header.SessionID == 0 {
		return proto.BuildErrorResponse(header, proto.StatusBadHeader), nil
	}

	offset, length, err := proto.ParseReadBody(body)
	if err != nil {
		return proto.BuildErrorResponse(header, proto.StatusBadBody), nil
	}

	mapped, ok := o.registry.Lookup(header.SessionID)
	if !ok {
		return proto.BuildErrorResponse(header, proto.StatusSessionUnavailable), nil
	}

	data, err := o.sessions.Read(mapped.UpstreamSession, offset, length)
	if err != nil {
		if err == session.ErrSessionUnavailable {
			o.registry.Close(header.SessionID)
		}
		return o.mapSessionError(header, err), nil
	}
	return proto.BuildSuccessResponse(header, data), nil
}

func (o *sessionOpener) handleWrite(header proto.Header, body []byte) ([]byte, error) {
	if header.SessionID == 0 {
		return proto.BuildErrorResponse(header, proto.StatusBadHeader), nil
	}

	offset, _, data, err := proto.ParseReadWriteBody(body)
	if err != nil {
		return proto.BuildErrorResponse(header, proto.StatusBadBody), nil
	}

	mapped, ok := o.registry.Lookup(header.SessionID)
	if !ok {
		return proto.BuildErrorResponse(header, proto.StatusSessionUnavailable), nil
	}

	if err := o.sessions.Write(mapped.UpstreamSession, offset, data); err != nil {
		if err == session.ErrSessionUnavailable {
			o.registry.Close(header.SessionID)
		}
		return o.mapSessionError(header, err), nil
	}
	return proto.BuildSuccessResponse(header, nil), nil
}

func (o *sessionOpener) closeConnection(connectionID uint64) {
	for _, mapped := range o.registry.CloseConnection(connectionID) {
		o.sessions.Close(mapped.UpstreamSession)
	}
	o.sessions.CloseConnection(connectionID)
}

func (o *sessionOpener) mapSessionError(header proto.Header, err error) []byte {
	switch err {
	case session.ErrSessionUnavailable:
		return proto.BuildErrorResponse(header, proto.StatusSessionUnavailable)
	case session.ErrSessionBusy:
		return proto.BuildErrorResponse(header, proto.StatusSessionBusy)
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
