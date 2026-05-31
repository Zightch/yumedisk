package storer

import (
	gatewayclient "yumedisk/server/internal/gateway/client"
	"yumedisk/server/internal/proto"
	"yumedisk/server/internal/route"
	"yumedisk/server/internal/session"
)

type dataPlane struct {
	links *activeLinks
}

func (r *Registry) DataPlane() gatewayclient.RouteSessionProxy {
	return &dataPlane{links: r.links}
}

func (p *dataPlane) Open(_ uint64, entry route.Entry) (uint64, error) {
	conn, ok := p.links.Lookup(entry.ConnectionID)
	if !ok {
		return 0, session.ErrSessionUnavailable
	}

	req := make([]byte, proto.HeaderSize)
	proto.EncodeHeader(proto.Header{
		ProtocolVersion: proto.ProtocolVersion,
		HeaderLen:       proto.HeaderSize,
		OpCode:          proto.OpSessionOpen,
		RequestID:       1,
	}, req)

	resp, err := conn.roundTrip(req)
	if err != nil {
		return 0, session.ErrSessionUnavailable
	}
	header, err := proto.ParseHeader(resp)
	if err != nil {
		return 0, session.ErrSessionUnavailable
	}
	if err := proto.ValidateResponseHeader(header); err != nil {
		return 0, session.ErrSessionUnavailable
	}
	if header.OpCode != proto.OpSessionOpen {
		return 0, session.ErrSessionUnavailable
	}
	if header.StatusCode != proto.StatusOK {
		return 0, mapResponseStatus(header.StatusCode)
	}
	if len(resp) != proto.HeaderSize {
		return 0, session.ErrSessionUnavailable
	}
	return header.SessionID, nil
}

func (p *dataPlane) RoundTrip(routeConnectionID uint64, sessionID uint64, opCode uint8, body []byte) (uint16, []byte, error) {
	conn, ok := p.links.Lookup(routeConnectionID)
	if !ok {
		return proto.StatusSessionUnavailable, nil, nil
	}

	resp, err := p.roundTripData(conn, opCode, sessionID, body)
	if err != nil {
		return proto.StatusSessionUnavailable, nil, nil
	}
	header, err := proto.ParseHeader(resp)
	if err != nil {
		return proto.StatusSessionUnavailable, nil, nil
	}
	if err := proto.ValidateResponseHeader(header); err != nil {
		return proto.StatusSessionUnavailable, nil, nil
	}
	if header.OpCode != opCode {
		return proto.StatusSessionUnavailable, nil, nil
	}
	responseBody := make([]byte, len(resp)-proto.HeaderSize)
	copy(responseBody, resp[proto.HeaderSize:])
	return header.StatusCode, responseBody, nil
}

func (p *dataPlane) SendNotice(routeConnectionID uint64, sessionID uint64, opCode uint8, body []byte) error {
	conn, ok := p.links.Lookup(routeConnectionID)
	if !ok {
		return nil
	}
	return conn.notify(proto.BuildNotice(
		opCode,
		sessionID,
		body,
	))
}

func (p *dataPlane) CloseConnection(uint64) {}

func (p *dataPlane) roundTripData(conn *connection, opCode uint8, sessionID uint64, body []byte) ([]byte, error) {
	req := make([]byte, proto.HeaderSize+len(body))
	proto.EncodeHeader(proto.Header{
		ProtocolVersion: proto.ProtocolVersion,
		HeaderLen:       proto.HeaderSize,
		OpCode:          opCode,
		RequestID:       1,
		SessionID:       sessionID,
	}, req)
	copy(req[proto.HeaderSize:], body)
	return conn.roundTrip(req)
}

func mapResponseStatus(status uint16) error {
	switch status {
	case proto.StatusIOReadOnly:
		return session.ErrReadOnly
	case proto.StatusIOLarge:
		return session.ErrIOLimit
	case proto.StatusIOOutOfRange:
		return session.ErrOutOfRange
	case proto.StatusIOFailed:
		return session.ErrIOFailed
	case proto.StatusSessionUnavailable:
		return session.ErrSessionUnavailable
	case proto.StatusSessionOpenRejected:
		return session.ErrSessionOpenRejected
	default:
		return session.ErrSessionUnavailable
	}
}
