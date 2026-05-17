package storer

import (
	"yumedisk/server/internal/session"
)

type localGatewayBackend struct {
	core *Core
}

func newLocalGatewayBackend(core *Core) *localGatewayBackend {
	return &localGatewayBackend{core: core}
}

func (b *localGatewayBackend) Open(connectionID uint64, diskID string) (session.Descriptor, error) {
	return b.core.SessionService().Open(connectionID, diskID)
}

func (b *localGatewayBackend) Ping(sessionID uint64) (session.Descriptor, bool) {
	return b.core.SessionService().Ping(sessionID)
}

func (b *localGatewayBackend) Close(sessionID uint64) {
	b.core.SessionService().Close(sessionID)
}

func (b *localGatewayBackend) CloseConnection(connectionID uint64) {
	b.core.SessionService().CloseConnection(connectionID)
}

func (b *localGatewayBackend) Read(sessionID uint64, offset uint64, length uint32) ([]byte, error) {
	return b.core.SessionService().Read(sessionID, offset, length)
}

func (b *localGatewayBackend) Write(sessionID uint64, offset uint64, data []byte) error {
	return b.core.SessionService().Write(sessionID, offset, data)
}

func (b *localGatewayBackend) TTLSeconds() uint32 {
	return b.core.SessionService().TTLSeconds()
}
