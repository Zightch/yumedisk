package storer

import (
	"yumedisk/server/internal/route"
)

type localGatewayBackend struct {
	core  *Core
	entry route.Entry
}

func newLocalGatewayBackend(core *Core) *localGatewayBackend {
	return &localGatewayBackend{
		core:  core,
		entry: core.RouteEntry("embedded://whole", 0),
	}
}

func (b *localGatewayBackend) LookupRoute(diskID string) (route.Entry, bool) {
	if diskID != b.entry.DiskID {
		return route.Entry{}, false
	}
	return b.entry, true
}

func (b *localGatewayBackend) Open(connectionID uint64, entry route.Entry) (uint64, error) {
	desc, err := b.core.SessionService().Open(connectionID)
	if err != nil {
		return 0, err
	}
	return desc.ID, nil
}

func (b *localGatewayBackend) Close(routeConnectionID uint64, sessionID uint64) {
	b.core.SessionService().Close(sessionID)
}

func (b *localGatewayBackend) CloseConnection(connectionID uint64) {
	b.core.SessionService().CloseConnection(connectionID)
}

func (b *localGatewayBackend) Read(routeConnectionID uint64, sessionID uint64, offset uint64, length uint32) ([]byte, error) {
	return b.core.SessionService().Read(sessionID, offset, length)
}

func (b *localGatewayBackend) Write(routeConnectionID uint64, sessionID uint64, offset uint64, data []byte) error {
	return b.core.SessionService().Write(sessionID, offset, data)
}
