package storer

import (
	gatewayclient "yumedisk/server/internal/gateway/client"
	"yumedisk/server/internal/proto"
	"yumedisk/server/internal/route"
	"yumedisk/server/internal/session"
)

type dataPlane struct {
	registry *Registry
}

func (r *Registry) DataPlane() gatewayclient.SessionDataPlane {
	return &dataPlane{registry: r}
}

func (p *dataPlane) Open(_ uint64, entry route.Entry) (uint64, error) {
	conn, ok := p.registry.connections.Lookup(entry.ConnectionID)
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
	if header.StatusCode != proto.StatusOK {
		return 0, mapResponseStatus(header.StatusCode)
	}
	if len(resp) != proto.HeaderSize {
		return 0, session.ErrSessionUnavailable
	}
	return header.SessionID, nil
}

func (p *dataPlane) Close(routeConnectionID uint64, sessionID uint64) {
	conn, ok := p.registry.connections.Lookup(routeConnectionID)
	if !ok {
		return
	}
	_, _ = p.roundTripData(conn, proto.OpClose, sessionID, nil)
}

func (p *dataPlane) CloseConnection(uint64) {}

func (p *dataPlane) Read(routeConnectionID uint64, sessionID uint64, offset uint64, length uint32) ([]byte, error) {
	conn, ok := p.registry.connections.Lookup(routeConnectionID)
	if !ok {
		return nil, session.ErrSessionUnavailable
	}

	resp, err := p.roundTripData(conn, proto.OpReadAt, sessionID, proto.BuildReadBody(offset, length))
	if err != nil {
		return nil, session.ErrSessionUnavailable
	}
	header, err := proto.ParseHeader(resp)
	if err != nil {
		return nil, session.ErrSessionUnavailable
	}
	if header.StatusCode != proto.StatusOK {
		return nil, mapResponseStatus(header.StatusCode)
	}
	data := make([]byte, len(resp)-proto.HeaderSize)
	copy(data, resp[proto.HeaderSize:])
	return data, nil
}

func (p *dataPlane) Write(routeConnectionID uint64, sessionID uint64, offset uint64, data []byte) error {
	conn, ok := p.registry.connections.Lookup(routeConnectionID)
	if !ok {
		return session.ErrSessionUnavailable
	}

	body := append(proto.BuildReadWriteBody(offset, uint32(len(data))), data...)
	resp, err := p.roundTripData(conn, proto.OpWriteAt, sessionID, body)
	if err != nil {
		return session.ErrSessionUnavailable
	}
	header, err := proto.ParseHeader(resp)
	if err != nil {
		return session.ErrSessionUnavailable
	}
	if header.StatusCode != proto.StatusOK {
		return mapResponseStatus(header.StatusCode)
	}
	return nil
}

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
