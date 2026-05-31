package client

import (
	"yumedisk/server/internal/route"
)

type RouteSource interface {
	LookupRoute(diskID string) (route.Entry, bool)
}

type RouteSessionProxy interface {
	Open(connectionID uint64, entry route.Entry) (uint64, error)
	RoundTrip(routeConnectionID uint64, sessionID uint64, opCode uint8, body []byte) (statusCode uint16, responseBody []byte, err error)
	SendNotice(routeConnectionID uint64, sessionID uint64, opCode uint8, body []byte) error
	CloseConnection(connectionID uint64)
}
