package clientauth

import "yumedisk/server/internal/route"

type RouteSource interface {
	LookupRoute(diskID string) (route.Entry, bool)
}

type ConnectionState interface {
	ConnectionID() uint64
	BeginAuth() error
	FinishAuth() error
	FailAuth()
	PendingAuth() bool
}
