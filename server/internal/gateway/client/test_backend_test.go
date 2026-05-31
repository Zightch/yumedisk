package client

import (
	"yumedisk/server/internal/auth"
	"yumedisk/server/internal/proto"
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
		DiskID:       material.DiskID,
		AuthVerifier: material.AuthVerifier,
		RouteTarget:  "test://local",
		ConnectionID: 0,
		Connected:    true,
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

func (b *testGatewayBackend) CloseConnection(connectionID uint64) {
	b.sessions.CloseConnection(connectionID)
}

func (b *testGatewayBackend) RoundTrip(routeConnectionID uint64, sessionID uint64, opCode uint8, body []byte) (uint16, []byte, error) {
	switch opCode {
	case proto.OpSessionDescribe:
		if sessionID == 0 {
			return proto.StatusBadHeader, nil, nil
		}
		if len(body) != 0 {
			return proto.StatusBadBody, nil, nil
		}
		metadata, err := b.sessions.Describe(sessionID)
		if err != nil {
			return mapTestSessionErrorStatus(err), nil, nil
		}
		return proto.StatusOK, proto.BuildSessionDescribeResponseBody(
			metadata.DiskSizeBytes,
			metadata.ReadOnly,
			metadata.BackendID,
		), nil
	case proto.OpReadAt:
		if sessionID == 0 {
			return proto.StatusBadHeader, nil, nil
		}
		offset, length, err := proto.ParseReadBody(body)
		if err != nil {
			return proto.StatusBadBody, nil, nil
		}
		data, err := b.sessions.Read(sessionID, offset, length)
		if err != nil {
			return mapTestSessionErrorStatus(err), nil, nil
		}
		return proto.StatusOK, proto.BuildReadResponseBody(data), nil
	case proto.OpWriteAt:
		if sessionID == 0 {
			return proto.StatusBadHeader, nil, nil
		}
		offset, _, data, err := proto.ParseReadWriteBody(body)
		if err != nil {
			return proto.StatusBadBody, nil, nil
		}
		if err := b.sessions.Write(sessionID, offset, data); err != nil {
			return mapTestSessionErrorStatus(err), nil, nil
		}
		return proto.StatusOK, nil, nil
	default:
		return proto.StatusUnsupportedOp, nil, nil
	}
}

func (b *testGatewayBackend) SendNotice(routeConnectionID uint64, sessionID uint64, opCode uint8, body []byte) error {
	switch opCode {
	case proto.OpSessionCloseNotice:
		if _, err := proto.ParseSessionCloseNoticeBody(body); err != nil {
			return err
		}
		b.sessions.Close(sessionID)
		return nil
	default:
		return nil
	}
}

func mapTestSessionErrorStatus(err error) uint16 {
	switch err {
	case session.ErrSessionUnavailable:
		return proto.StatusSessionUnavailable
	case session.ErrSessionOpenRejected:
		return proto.StatusSessionOpenRejected
	case session.ErrReadOnly:
		return proto.StatusIOReadOnly
	case session.ErrIOLimit:
		return proto.StatusIOLarge
	case session.ErrOutOfRange:
		return proto.StatusIOOutOfRange
	case session.ErrIOFailed:
		return proto.StatusIOFailed
	default:
		return proto.StatusIOFailed
	}
}
