package client

import (
	"yumedisk/server/internal/auth"
	"yumedisk/server/internal/route"
	"yumedisk/server/internal/session"
)

type testGatewayBackend struct {
	sessions *session.Service
	routes   *route.Registry
}

func newTestGatewayBackend(material auth.Material, sessions *session.Service, diskSize uint64, readOnly bool) *testGatewayBackend {
	routes := route.NewRegistry()
	_ = routes.Register(route.Entry{
		DiskID:        material.DiskID,
		AuthVerifier:  material.AuthVerifier,
		RouteTarget:   "test://local",
		ConnectionID:  0,
		Connected:     true,
		DiskSizeBytes: diskSize,
		ReadOnly:      readOnly,
		MaxIOBytes:    sessions.MaxIOBytes(),
	})
	return &testGatewayBackend{
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

func (b *testGatewayBackend) Open(connectionID uint64, entry route.Entry) (uint64, error) {
	desc, err := b.sessions.Open(connectionID)
	if err != nil {
		return 0, err
	}
	return desc.ID, nil
}

func (b *testGatewayBackend) Close(routeConnectionID uint64, sessionID uint64) {
	b.sessions.Close(sessionID)
}

func (b *testGatewayBackend) CloseConnection(connectionID uint64) {
	b.sessions.CloseConnection(connectionID)
}

func (b *testGatewayBackend) Read(routeConnectionID uint64, sessionID uint64, offset uint64, length uint32) ([]byte, error) {
	return b.sessions.Read(sessionID, offset, length)
}

func (b *testGatewayBackend) Write(routeConnectionID uint64, sessionID uint64, offset uint64, data []byte) error {
	return b.sessions.Write(sessionID, offset, data)
}
