package gateway

import (
	"yumedisk/server/internal/auth"
	"yumedisk/server/internal/session"
)

type testGatewayBackend struct {
	material auth.Material
	sessions *session.Service
}

func newTestGatewayBackend(material auth.Material, sessions *session.Service) *testGatewayBackend {
	return &testGatewayBackend{
		material: material,
		sessions: sessions,
	}
}

func (b *testGatewayBackend) LookupAuthVerifier(diskID string) ([64]byte, bool) {
	if diskID != b.material.DiskID {
		return [64]byte{}, false
	}
	return b.material.AuthVerifier, true
}

func (b *testGatewayBackend) Open(connectionID uint64, diskID string) (session.Descriptor, error) {
	return b.sessions.Open(connectionID, diskID)
}

func (b *testGatewayBackend) Ping(sessionID uint64) (session.Descriptor, bool) {
	return b.sessions.Ping(sessionID)
}

func (b *testGatewayBackend) Close(sessionID uint64) {
	b.sessions.Close(sessionID)
}

func (b *testGatewayBackend) CloseConnection(connectionID uint64) {
	b.sessions.CloseConnection(connectionID)
}

func (b *testGatewayBackend) Read(sessionID uint64, offset uint64, length uint32) ([]byte, error) {
	return b.sessions.Read(sessionID, offset, length)
}

func (b *testGatewayBackend) Write(sessionID uint64, offset uint64, data []byte) error {
	return b.sessions.Write(sessionID, offset, data)
}

func (b *testGatewayBackend) TTLSeconds() uint32 {
	return b.sessions.TTLSeconds()
}
