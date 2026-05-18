package gateway

import (
	"net"
	"sync"

	"yumedisk/server/internal/proto"
	"yumedisk/server/internal/route"
	"yumedisk/server/internal/session"
)

type StorerRouteRegistry struct {
	routes *route.Registry

	handlerMu sync.RWMutex
	handler   routeDisconnectHandler

	connections *storerConnectionRegistry
}

type routeDisconnectHandler interface {
	CloseRouteConnection(routeConnectionID uint64, diskIDs []string)
}

func NewStorerRouteRegistry() *StorerRouteRegistry {
	return &StorerRouteRegistry{
		routes:      route.NewRegistry(),
		connections: newStorerConnectionRegistry(),
	}
}

func (r *StorerRouteRegistry) SetDisconnectHandler(handler routeDisconnectHandler) {
	r.handlerMu.Lock()
	r.handler = handler
	r.handlerMu.Unlock()
}

func (r *StorerRouteRegistry) LookupRoute(diskID string) (route.Entry, bool) {
	return r.routes.LookupRoute(diskID)
}

func (r *StorerRouteRegistry) Register(entry route.Entry) error {
	return r.routes.Register(entry)
}

func (r *StorerRouteRegistry) AttachConnection(connectionID uint64, conn net.Conn) *storerConnection {
	return r.connections.Attach(connectionID, conn)
}

func (r *StorerRouteRegistry) DisconnectConnection(connectionID uint64) {
	disconnected := r.routes.DisconnectConnection(connectionID)
	conn := r.connections.Remove(connectionID)

	if conn != nil {
		conn.closePending()
	}

	r.handlerMu.RLock()
	handler := r.handler
	r.handlerMu.RUnlock()
	if handler != nil {
		diskIDs := make([]string, 0, len(disconnected))
		for _, entry := range disconnected {
			diskIDs = append(diskIDs, entry.DiskID)
		}
		handler.CloseRouteConnection(connectionID, diskIDs)
	}
}

func (r *StorerRouteRegistry) connection(connectionID uint64) (*storerConnection, bool) {
	return r.connections.Lookup(connectionID)
}

func (r *StorerRouteRegistry) Open(clientConnectionID uint64, entry route.Entry) (uint64, error) {
	conn, ok := r.connection(entry.ConnectionID)
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

func (r *StorerRouteRegistry) Close(routeConnectionID uint64, sessionID uint64) {
	_, _ = r.roundTripData(routeConnectionID, proto.OpClose, sessionID, nil)
}

func (r *StorerRouteRegistry) CloseConnection(uint64) {}

func (r *StorerRouteRegistry) Read(routeConnectionID uint64, sessionID uint64, offset uint64, length uint32) ([]byte, error) {
	resp, err := r.roundTripData(routeConnectionID, proto.OpReadAt, sessionID, proto.BuildReadBody(offset, length))
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

func (r *StorerRouteRegistry) Write(routeConnectionID uint64, sessionID uint64, offset uint64, data []byte) error {
	body := append(proto.BuildReadWriteBody(offset, uint32(len(data))), data...)
	resp, err := r.roundTripData(routeConnectionID, proto.OpWriteAt, sessionID, body)
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

func (r *StorerRouteRegistry) roundTripData(routeConnectionID uint64, opCode uint8, sessionID uint64, body []byte) ([]byte, error) {
	conn, ok := r.connection(routeConnectionID)
	if !ok {
		return nil, session.ErrSessionUnavailable
	}

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
