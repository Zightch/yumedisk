package gateway

import (
	"yumedisk/server/internal/route"
	"yumedisk/server/internal/session"
)

type RouteSource interface {
	LookupRoute(diskID string) (route.Entry, bool)
}

type SessionDataPlane interface {
	Open(connectionID uint64, diskID string) (session.Descriptor, error)
	Ping(routeConnectionID uint64, sessionID uint64) (session.Descriptor, bool)
	Close(routeConnectionID uint64, sessionID uint64)
	CloseConnection(connectionID uint64)
	Read(routeConnectionID uint64, sessionID uint64, offset uint64, length uint32) ([]byte, error)
	Write(routeConnectionID uint64, sessionID uint64, offset uint64, data []byte) error
}
