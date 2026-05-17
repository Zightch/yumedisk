package gateway

import (
	"testing"

	"yumedisk/server/internal/proto"
	"yumedisk/server/internal/route"
	"yumedisk/server/internal/session"
)

func TestGatewaySessionMappingHidesUpstreamSessionID(t *testing.T) {
	t.Parallel()

	diskID := "DISK000000000001"
	routes := &mappingRouteSource{
		entry: route.Entry{
			DiskID:       diskID,
			RouteTarget:  "storer://conn-9",
			ConnectionID: 9,
			Connected:    true,
		},
	}
	dataPlane := &mappingDataPlane{
		openDesc: session.Descriptor{
			ID:         77,
			DiskID:     diskID,
			DiskSize:   4096,
			MaxIOBytes: 1024,
			ReadOnly:   false,
		},
		pingOK:  true,
		readOut: []byte("OK"),
	}

	handler, err := NewHandler(routes, dataPlane)
	if err != nil {
		t.Fatalf("new handler: %v", err)
	}
	state := handler.NewConnectionState(42)
	state.markAuthenticated(diskID)

	openResp, err := handler.HandlePayload(state, buildRequest(proto.OpSessionOpen, 1, 0, []byte(diskID)))
	if err != nil {
		t.Fatalf("open session: %v", err)
	}
	openHeader, err := proto.ParseHeader(openResp)
	if err != nil {
		t.Fatalf("parse open response: %v", err)
	}
	if openHeader.StatusCode != proto.StatusOK {
		t.Fatalf("unexpected open status: %d", openHeader.StatusCode)
	}
	if openHeader.SessionID == 0 || openHeader.SessionID == dataPlane.openDesc.ID {
		t.Fatalf("gateway session id leaked upstream id: got=%d upstream=%d", openHeader.SessionID, dataPlane.openDesc.ID)
	}

	pingResp, err := handler.HandlePayload(state, buildRequest(proto.OpPing, 2, openHeader.SessionID, proto.BuildPingResponseBody(9)))
	if err != nil {
		t.Fatalf("ping: %v", err)
	}
	pingHeader, err := proto.ParseHeader(pingResp)
	if err != nil {
		t.Fatalf("parse ping response: %v", err)
	}
	if pingHeader.StatusCode != proto.StatusOK {
		t.Fatalf("unexpected ping status: %d", pingHeader.StatusCode)
	}
	if dataPlane.lastPingSessionID != dataPlane.openDesc.ID {
		t.Fatalf("ping used wrong upstream session id: %d", dataPlane.lastPingSessionID)
	}

	readResp, err := handler.HandlePayload(state, buildRequest(proto.OpReadAt, 3, openHeader.SessionID, proto.BuildReadBody(0, 2)))
	if err != nil {
		t.Fatalf("read: %v", err)
	}
	readHeader, err := proto.ParseHeader(readResp)
	if err != nil {
		t.Fatalf("parse read response: %v", err)
	}
	if readHeader.StatusCode != proto.StatusOK {
		t.Fatalf("unexpected read status: %d", readHeader.StatusCode)
	}
	if dataPlane.lastReadSessionID != dataPlane.openDesc.ID {
		t.Fatalf("read used wrong upstream session id: %d", dataPlane.lastReadSessionID)
	}

	writePayload := append(proto.BuildReadWriteBody(0, 2), []byte("HI")...)
	writeResp, err := handler.HandlePayload(state, buildRequest(proto.OpWriteAt, 4, openHeader.SessionID, writePayload))
	if err != nil {
		t.Fatalf("write: %v", err)
	}
	writeHeader, err := proto.ParseHeader(writeResp)
	if err != nil {
		t.Fatalf("parse write response: %v", err)
	}
	if writeHeader.StatusCode != proto.StatusOK {
		t.Fatalf("unexpected write status: %d", writeHeader.StatusCode)
	}
	if dataPlane.lastWriteSessionID != dataPlane.openDesc.ID {
		t.Fatalf("write used wrong upstream session id: %d", dataPlane.lastWriteSessionID)
	}

	closeResp, err := handler.HandlePayload(state, buildRequest(proto.OpClose, 5, openHeader.SessionID, nil))
	if err != nil {
		t.Fatalf("close: %v", err)
	}
	closeHeader, err := proto.ParseHeader(closeResp)
	if err != nil {
		t.Fatalf("parse close response: %v", err)
	}
	if closeHeader.StatusCode != proto.StatusOK {
		t.Fatalf("unexpected close status: %d", closeHeader.StatusCode)
	}
	if dataPlane.lastCloseSessionID != dataPlane.openDesc.ID {
		t.Fatalf("close used wrong upstream session id: %d", dataPlane.lastCloseSessionID)
	}
}

func TestGatewaySessionMappingIsReleasedOnConnectionClose(t *testing.T) {
	t.Parallel()

	diskID := "DISK000000000001"
	routes := &mappingRouteSource{
		entry: route.Entry{
			DiskID:       diskID,
			RouteTarget:  "storer://conn-11",
			ConnectionID: 11,
			Connected:    true,
		},
	}
	dataPlane := &mappingDataPlane{
		openDesc: session.Descriptor{
			ID:         88,
			DiskID:     diskID,
			DiskSize:   4096,
			MaxIOBytes: 1024,
		},
		pingOK: true,
	}

	handler, err := NewHandler(routes, dataPlane)
	if err != nil {
		t.Fatalf("new handler: %v", err)
	}
	state := handler.NewConnectionState(55)
	state.markAuthenticated(diskID)

	openResp, err := handler.HandlePayload(state, buildRequest(proto.OpSessionOpen, 10, 0, []byte(diskID)))
	if err != nil {
		t.Fatalf("open session: %v", err)
	}
	openHeader, err := proto.ParseHeader(openResp)
	if err != nil {
		t.Fatalf("parse open response: %v", err)
	}

	handler.CloseConnection(state.ID)
	if dataPlane.lastCloseSessionID != dataPlane.openDesc.ID {
		t.Fatalf("close connection did not release upstream session: %d", dataPlane.lastCloseSessionID)
	}
	if dataPlane.lastCloseConnectionID != state.ID {
		t.Fatalf("close connection used wrong id: %d", dataPlane.lastCloseConnectionID)
	}

	pingResp, err := handler.HandlePayload(state, buildRequest(proto.OpPing, 11, openHeader.SessionID, proto.BuildPingResponseBody(1)))
	if err != nil {
		t.Fatalf("ping after connection close: %v", err)
	}
	pingHeader, err := proto.ParseHeader(pingResp)
	if err != nil {
		t.Fatalf("parse ping-after-close response: %v", err)
	}
	if pingHeader.StatusCode != proto.StatusSessionUnavailable {
		t.Fatalf("unexpected ping-after-close status: %d", pingHeader.StatusCode)
	}
}

type mappingRouteSource struct {
	entry route.Entry
}

func (s *mappingRouteSource) LookupRoute(diskID string) (route.Entry, bool) {
	if diskID != s.entry.DiskID {
		return route.Entry{}, false
	}
	return s.entry, true
}

type mappingDataPlane struct {
	openDesc              session.Descriptor
	pingOK                bool
	readOut               []byte
	openErr               error
	readErr               error
	writeErr              error
	lastPingSessionID     uint64
	lastReadSessionID     uint64
	lastWriteSessionID    uint64
	lastCloseSessionID    uint64
	lastCloseConnectionID uint64
}

func (p *mappingDataPlane) Open(uint64, string) (session.Descriptor, error) {
	return p.openDesc, p.openErr
}

func (p *mappingDataPlane) Ping(routeConnectionID uint64, sessionID uint64) (session.Descriptor, bool) {
	p.lastPingSessionID = sessionID
	return p.openDesc, p.pingOK
}

func (p *mappingDataPlane) Close(routeConnectionID uint64, sessionID uint64) {
	p.lastCloseSessionID = sessionID
}

func (p *mappingDataPlane) CloseConnection(connectionID uint64) {
	p.lastCloseConnectionID = connectionID
}

func (p *mappingDataPlane) Read(routeConnectionID uint64, sessionID uint64, offset uint64, length uint32) ([]byte, error) {
	p.lastReadSessionID = sessionID
	return p.readOut, p.readErr
}

func (p *mappingDataPlane) Write(routeConnectionID uint64, sessionID uint64, offset uint64, data []byte) error {
	p.lastWriteSessionID = sessionID
	return p.writeErr
}
