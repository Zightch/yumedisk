package gateway

import (
	"yumedisk/server/internal/auth"
	"yumedisk/server/internal/route"
	"yumedisk/server/internal/session"
)

type testGatewayBackend struct {
	material auth.Material
	sessions *session.Service
	routes   *route.Registry
}

func newTestGatewayBackend(material auth.Material, sessions *session.Service) *testGatewayBackend {
	routes := route.NewRegistry()
	_ = routes.Register(route.Entry{
		DiskID:            material.DiskID,
		AuthVerifier:      material.AuthVerifier,
		RouteTarget:       "test://local",
		ConnectionID:      0,
		Connected:         true,
		MaxIOBytes:        sessions.MaxIOBytes(),
		SessionTTLSeconds: sessions.TTLSeconds(),
	})
	return &testGatewayBackend{
		material: material,
		sessions: sessions,
		routes:   routes,
	}
}

func (b *testGatewayBackend) LookupRoute(diskID string) (route.Entry, bool) {
	return b.routes.LookupRoute(diskID)
}

func (b *testGatewayBackend) DisconnectRoute() {
	b.routes.DisconnectConnection(0)
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
