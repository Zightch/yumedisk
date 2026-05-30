package client

import (
	"sync"
	"testing"
	"time"

	"yumedisk/server/internal/proto"
	"yumedisk/server/internal/route"
	"yumedisk/server/internal/session"
)

func TestGatewaySessionMappingHidesUpstreamSessionIDAndForwardsDescribe(t *testing.T) {
	t.Parallel()

	const diskID = "DISK000000000001"
	routes := &mappingRouteSource{
		entry: route.Entry{
			DiskID:       diskID,
			RouteTarget:  "storer://conn-9",
			ConnectionID: 9,
			Connected:    true,
		},
	}
	dataPlane := &mappingDataPlane{
		openSessionID: 77,
		readOut:       []byte("OK"),
		describeOut: session.Metadata{
			DiskSizeBytes: 4096,
			MaxIOBytes:    1024,
		},
	}

	handler, err := NewHandler(routes, dataPlane)
	if err != nil {
		t.Fatalf("new handler: %v", err)
	}
	state := handler.NewConnectionState(42)
	authID := handler.grants.Issue(state.ConnectionID(), diskID, time.Now().Add(time.Minute))

	openResp, err := handler.HandlePayload(state, buildRequest(proto.OpSessionOpen, 1, 0, proto.BuildSessionOpenRequestBody(authID)))
	if err != nil {
		t.Fatalf("open session: %v", err)
	}
	openHeader := mustParseGatewayHeader(t, openResp)
	if openHeader.StatusCode != proto.StatusOK {
		t.Fatalf("unexpected open status: %d", openHeader.StatusCode)
	}
	if openHeader.SessionID == 0 || openHeader.SessionID == dataPlane.openSessionID {
		t.Fatalf("gateway session id leaked upstream id: got=%d upstream=%d", openHeader.SessionID, dataPlane.openSessionID)
	}

	describeResp, err := handler.HandlePayload(state, buildRequest(proto.OpSessionDescribe, 2, openHeader.SessionID, nil))
	if err != nil {
		t.Fatalf("describe: %v", err)
	}
	describeHeader := mustParseGatewayHeader(t, describeResp)
	if describeHeader.StatusCode != proto.StatusOK {
		t.Fatalf("unexpected describe status: %d", describeHeader.StatusCode)
	}
	diskSize, maxIOBytes, readOnly, backendID, err := proto.ParseSessionDescribeResponseBody(describeResp[proto.HeaderSize:])
	if err != nil {
		t.Fatalf("parse describe body: %v", err)
	}
	if diskSize != dataPlane.describeOut.DiskSizeBytes || maxIOBytes != dataPlane.describeOut.MaxIOBytes || readOnly != dataPlane.describeOut.ReadOnly {
		t.Fatalf("unexpected describe body: size=%d maxIO=%d readOnly=%v", diskSize, maxIOBytes, readOnly)
	}
	if backendID != dataPlane.describeOut.BackendID {
		t.Fatal("unexpected backend id")
	}
	if dataPlane.lastDescribeSessionID != dataPlane.openSessionID {
		t.Fatalf("describe used wrong upstream session id: %d", dataPlane.lastDescribeSessionID)
	}

	readResp, err := handler.HandlePayload(state, buildRequest(proto.OpReadAt, 3, openHeader.SessionID, proto.BuildReadBody(0, 2)))
	if err != nil {
		t.Fatalf("read: %v", err)
	}
	if header := mustParseGatewayHeader(t, readResp); header.StatusCode != proto.StatusOK {
		t.Fatalf("unexpected read status: %d", header.StatusCode)
	}
	if dataPlane.lastReadSessionID != dataPlane.openSessionID {
		t.Fatalf("read used wrong upstream session id: %d", dataPlane.lastReadSessionID)
	}

	writePayload := append(proto.BuildReadWriteBody(0, 2), []byte("HI")...)
	writeResp, err := handler.HandlePayload(state, buildRequest(proto.OpWriteAt, 4, openHeader.SessionID, writePayload))
	if err != nil {
		t.Fatalf("write: %v", err)
	}
	if header := mustParseGatewayHeader(t, writeResp); header.StatusCode != proto.StatusOK {
		t.Fatalf("unexpected write status: %d", header.StatusCode)
	}
	if dataPlane.lastWriteSessionID != dataPlane.openSessionID {
		t.Fatalf("write used wrong upstream session id: %d", dataPlane.lastWriteSessionID)
	}

	closeResp, err := handler.HandlePayload(state, buildNotice(
		proto.OpSessionCloseNotice,
		openHeader.SessionID,
		proto.BuildSessionCloseNoticeBody(proto.SessionCloseReasonNormalClose),
	))
	if err != nil {
		t.Fatalf("close notice: %v", err)
	}
	if closeResp != nil {
		t.Fatal("expected close notice to produce no response")
	}
	if dataPlane.lastCloseSessionID != dataPlane.openSessionID {
		t.Fatalf("close used wrong upstream session id: %d", dataPlane.lastCloseSessionID)
	}
}

func TestGatewaySessionMappingIsReleasedOnConnectionClose(t *testing.T) {
	t.Parallel()

	const diskID = "DISK000000000001"
	routes := &mappingRouteSource{
		entry: route.Entry{
			DiskID:       diskID,
			RouteTarget:  "storer://conn-11",
			ConnectionID: 11,
			Connected:    true,
		},
	}
	dataPlane := &mappingDataPlane{
		openSessionID: 88,
		describeOut: session.Metadata{
			DiskSizeBytes: 4096,
			MaxIOBytes:    1024,
		},
	}

	handler, err := NewHandler(routes, dataPlane)
	if err != nil {
		t.Fatalf("new handler: %v", err)
	}
	state := handler.NewConnectionState(55)
	authID := handler.grants.Issue(state.ConnectionID(), diskID, time.Now().Add(time.Minute))

	openResp, err := handler.HandlePayload(state, buildRequest(proto.OpSessionOpen, 10, 0, proto.BuildSessionOpenRequestBody(authID)))
	if err != nil {
		t.Fatalf("open session: %v", err)
	}
	openHeader := mustParseGatewayHeader(t, openResp)

	handler.CloseConnection(state.ConnectionID())
	if dataPlane.lastCloseSessionID != dataPlane.openSessionID {
		t.Fatalf("close connection did not release upstream session: %d", dataPlane.lastCloseSessionID)
	}
	if dataPlane.lastCloseConnectionID != state.ConnectionID() {
		t.Fatalf("close connection used wrong id: %d", dataPlane.lastCloseConnectionID)
	}

	readResp, err := handler.HandlePayload(state, buildRequest(proto.OpReadAt, 11, openHeader.SessionID, proto.BuildReadBody(0, 1)))
	if err != nil {
		t.Fatalf("read after connection close: %v", err)
	}
	if header := mustParseGatewayHeader(t, readResp); header.StatusCode != proto.StatusSessionUnavailable {
		t.Fatalf("unexpected read-after-close status: %d", header.StatusCode)
	}
}

func TestGatewayRouteDisconnectClosesClientSessionAndRevokesGrant(t *testing.T) {
	t.Parallel()

	const diskID = "DISK000000000001"
	routes := &mappingRouteSource{
		entry: route.Entry{
			DiskID:       diskID,
			RouteTarget:  "storer://conn-15",
			ConnectionID: 15,
			Connected:    true,
		},
	}
	dataPlane := &mappingDataPlane{openSessionID: 91}

	handler, err := NewHandler(routes, dataPlane)
	if err != nil {
		t.Fatalf("new handler: %v", err)
	}
	notifier := &recordingSessionCloseNotifier{}
	handler.SetSessionCloseNotifier(notifier)
	state := handler.NewConnectionState(77)
	authID := handler.grants.Issue(state.ConnectionID(), diskID, time.Now().Add(time.Minute))
	openResp, err := handler.HandlePayload(state, buildRequest(proto.OpSessionOpen, 1, 0, proto.BuildSessionOpenRequestBody(authID)))
	if err != nil {
		t.Fatalf("open session: %v", err)
	}
	openHeader := mustParseGatewayHeader(t, openResp)
	if openHeader.StatusCode != proto.StatusOK {
		t.Fatalf("unexpected open status: %d", openHeader.StatusCode)
	}

	pendingAuthID := handler.grants.Issue(state.ConnectionID(), diskID, time.Now().Add(time.Minute))
	closed := handler.closeRouteConnectionSessions(routes.entry.ConnectionID, []string{diskID})
	if len(closed) != 1 {
		t.Fatalf("unexpected closed sessions count: %d", len(closed))
	}
	if closed[0].ClientConnectionID != state.ConnectionID() {
		t.Fatalf("unexpected client connection id: %d", closed[0].ClientConnectionID)
	}
	if notifier.count() != 0 {
		t.Fatalf("manual closeRouteConnectionSessions should not emit notices")
	}

	if _, status, ok := handler.grants.Lookup(pendingAuthID, state.ConnectionID()); ok || status != proto.StatusAuthIDInvalid {
		t.Fatalf("expected route disconnect to revoke auth grant, ok=%v status=%d", ok, status)
	}

	pendingAuthID = handler.grants.Issue(state.ConnectionID(), diskID, time.Now().Add(time.Minute))
	authID = handler.grants.Issue(state.ConnectionID(), diskID, time.Now().Add(time.Minute))
	openResp, err = handler.HandlePayload(state, buildRequest(proto.OpSessionOpen, 2, 0, proto.BuildSessionOpenRequestBody(authID)))
	if err != nil {
		t.Fatalf("open session for notifier path: %v", err)
	}
	openHeader = mustParseGatewayHeader(t, openResp)
	if openHeader.StatusCode != proto.StatusOK {
		t.Fatalf("unexpected notifier-path open status: %d", openHeader.StatusCode)
	}
	handler.CloseRouteConnection(routes.entry.ConnectionID, []string{diskID})
	if notifier.count() != 1 {
		t.Fatalf("expected one route-lost notice, got %d", notifier.count())
	}
	record := notifier.last()
	if record.reason != proto.SessionCloseReasonRouteLost {
		t.Fatalf("unexpected close reason: %d", record.reason)
	}
	if record.sessionID != openHeader.SessionID {
		t.Fatalf("unexpected closed session id: %d", record.sessionID)
	}
	if record.clientConnectionID != state.ConnectionID() {
		t.Fatalf("unexpected closed client connection id: %d", record.clientConnectionID)
	}
	if _, status, ok := handler.grants.Lookup(pendingAuthID, state.ConnectionID()); ok || status != proto.StatusAuthIDInvalid {
		t.Fatalf("expected notifier path to revoke auth grant, ok=%v status=%d", ok, status)
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
	openSessionID         uint64
	openErr               error
	describeOut           session.Metadata
	describeErr           error
	readOut               []byte
	readErr               error
	writeErr              error
	lastDescribeSessionID uint64
	lastReadSessionID     uint64
	lastWriteSessionID    uint64
	lastCloseSessionID    uint64
	lastCloseConnectionID uint64
}

func (p *mappingDataPlane) Open(uint64, route.Entry) (uint64, error) {
	return p.openSessionID, p.openErr
}

func (p *mappingDataPlane) Describe(routeConnectionID uint64, sessionID uint64) (session.Metadata, error) {
	p.lastDescribeSessionID = sessionID
	return p.describeOut, p.describeErr
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

type recordingSessionCloseNotifier struct {
	mu      sync.Mutex
	records []sessionCloseRecord
}

type sessionCloseRecord struct {
	sessionID          uint64
	clientConnectionID uint64
	reason             uint16
}

func (n *recordingSessionCloseNotifier) NotifySessionClosed(sessionID uint64, clientConnectionID uint64, reason uint16) {
	n.mu.Lock()
	n.records = append(n.records, sessionCloseRecord{
		sessionID:          sessionID,
		clientConnectionID: clientConnectionID,
		reason:             reason,
	})
	n.mu.Unlock()
}

func (n *recordingSessionCloseNotifier) count() int {
	n.mu.Lock()
	defer n.mu.Unlock()
	return len(n.records)
}

func (n *recordingSessionCloseNotifier) last() sessionCloseRecord {
	n.mu.Lock()
	defer n.mu.Unlock()
	return n.records[len(n.records)-1]
}
