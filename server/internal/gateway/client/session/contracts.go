package session

import "yumedisk/server/internal/route"

type RouteSource interface {
	LookupRoute(diskID string) (route.Entry, bool)
}

type DataPlane interface {
	Open(connectionID uint64, entry route.Entry) (uint64, error)
	Close(routeConnectionID uint64, sessionID uint64)
	CloseConnection(connectionID uint64)
	Read(routeConnectionID uint64, sessionID uint64, offset uint64, length uint32) ([]byte, error)
	Write(routeConnectionID uint64, sessionID uint64, offset uint64, data []byte) error
}

type GrantRegistry interface {
	LookupDisk(authID uint64, connectionID uint64) (string, uint16, bool)
	ConsumeDisk(authID uint64) (string, bool)
}

type ConnectionState interface {
	ConnectionID() uint64
	BeginSessionOpen() error
	FinishSessionOpen() error
	FailSessionOpen()
	MarkHeartbeat()
}
