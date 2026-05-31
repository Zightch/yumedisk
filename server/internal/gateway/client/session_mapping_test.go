package client

import (
	"bytes"
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
	expectedDescribeBody := proto.BuildSessionDescribeResponseBody(
		dataPlane.describeOut.DiskSizeBytes,
		dataPlane.describeOut.MaxIOBytes,
		dataPlane.describeOut.ReadOnly,
		dataPlane.describeOut.BackendID,
	)
	if !bytes.Equal(describeResp[proto.HeaderSize:], expectedDescribeBody) {
		t.Fatalf("unexpected describe body bytes: got=%v want=%v", describeResp[proto.HeaderSize:], expectedDescribeBody)
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
	if !bytes.Equal(readResp[proto.HeaderSize:], dataPlane.readOut) {
		t.Fatalf("unexpected read body bytes: got=%v want=%v", readResp[proto.HeaderSize:], dataPlane.readOut)
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
	if !bytes.Equal(dataPlane.lastWriteBody, writePayload) {
		t.Fatalf("unexpected write request body bytes: got=%v want=%v", dataPlane.lastWriteBody, writePayload)
	}

	closeResp, err := handler.HandlePayload(state, buildNotice(
		proto.OpSessionCloseNotice,
		openHeader.SessionID,
		proto.BuildSessionCloseNoticeBody(proto.SessionCloseReasonGatewayShutdown),
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
	if reason, err := proto.ParseSessionCloseNoticeBody(dataPlane.lastCloseBody); err != nil || reason != proto.SessionCloseReasonGatewayShutdown {
		t.Fatalf("unexpected bridged close body: reason=%d err=%v", reason, err)
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
	if reason, err := proto.ParseSessionCloseNoticeBody(dataPlane.lastCloseBody); err != nil || reason != proto.SessionCloseReasonNormalClose {
		t.Fatalf("unexpected synthesized close body on disconnect: reason=%d err=%v", reason, err)
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
	reason, err := proto.ParseSessionCloseNoticeBody(record.body)
	if err != nil {
		t.Fatalf("parse route-lost close body: %v", err)
	}
	if reason != proto.SessionCloseReasonRouteLost {
		t.Fatalf("unexpected close reason: %d", reason)
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

func TestGatewayRouteCloseNoticePreservesBodyAndReleasesSession(t *testing.T) {
	t.Parallel()

	const diskID = "DISK000000000001"
	routes := &mappingRouteSource{
		entry: route.Entry{
			DiskID:       diskID,
			RouteTarget:  "storer://conn-21",
			ConnectionID: 21,
			Connected:    true,
		},
	}
	dataPlane := &mappingDataPlane{
		openSessionID: 123,
		readOut:       []byte("OK"),
	}

	handler, err := NewHandler(routes, dataPlane)
	if err != nil {
		t.Fatalf("new handler: %v", err)
	}
	notifier := &recordingSessionCloseNotifier{}
	handler.SetSessionCloseNotifier(notifier)
	state := handler.NewConnectionState(90)
	authID := handler.grants.Issue(state.ConnectionID(), diskID, time.Now().Add(time.Minute))

	openResp, err := handler.HandlePayload(state, buildRequest(proto.OpSessionOpen, 1, 0, proto.BuildSessionOpenRequestBody(authID)))
	if err != nil {
		t.Fatalf("open session: %v", err)
	}
	openHeader := mustParseGatewayHeader(t, openResp)
	if openHeader.StatusCode != proto.StatusOK {
		t.Fatalf("unexpected open status: %d", openHeader.StatusCode)
	}

	closeBody := proto.BuildSessionCloseNoticeBody(proto.SessionCloseReasonGatewayShutdown)
	handler.NotifyRouteSessionClosed(routes.entry.ConnectionID, dataPlane.openSessionID, closeBody)

	if notifier.count() != 1 {
		t.Fatalf("expected one upstream close notice, got %d", notifier.count())
	}
	record := notifier.last()
	if record.sessionID != openHeader.SessionID {
		t.Fatalf("unexpected closed session id: %d", record.sessionID)
	}
	if record.clientConnectionID != state.ConnectionID() {
		t.Fatalf("unexpected closed client connection id: %d", record.clientConnectionID)
	}
	if !bytes.Equal(record.body, closeBody) {
		t.Fatalf("unexpected close body: got=%v want=%v", record.body, closeBody)
	}

	readResp, err := handler.HandlePayload(state, buildRequest(proto.OpReadAt, 2, openHeader.SessionID, proto.BuildReadBody(0, 1)))
	if err != nil {
		t.Fatalf("read after upstream close: %v", err)
	}
	if header := mustParseGatewayHeader(t, readResp); header.StatusCode != proto.StatusSessionUnavailable {
		t.Fatalf("unexpected read-after-upstream-close status: %d", header.StatusCode)
	}
}

func TestGatewayRoundTripPassesThroughUnknownStatusCode(t *testing.T) {
	t.Parallel()

	const diskID = "DISK000000000001"
	routes := &mappingRouteSource{
		entry: route.Entry{
			DiskID:       diskID,
			RouteTarget:  "storer://conn-31",
			ConnectionID: 31,
			Connected:    true,
		},
	}
	dataPlane := &mappingDataPlane{
		openSessionID: 144,
		statusByOp: map[uint8]uint16{
			proto.OpReadAt: 0x7E01,
		},
	}

	handler, err := NewHandler(routes, dataPlane)
	if err != nil {
		t.Fatalf("new handler: %v", err)
	}
	state := handler.NewConnectionState(91)
	authID := handler.grants.Issue(state.ConnectionID(), diskID, time.Now().Add(time.Minute))

	openResp, err := handler.HandlePayload(state, buildRequest(proto.OpSessionOpen, 1, 0, proto.BuildSessionOpenRequestBody(authID)))
	if err != nil {
		t.Fatalf("open session: %v", err)
	}
	openHeader := mustParseGatewayHeader(t, openResp)
	if openHeader.StatusCode != proto.StatusOK {
		t.Fatalf("unexpected open status: %d", openHeader.StatusCode)
	}

	readResp, err := handler.HandlePayload(state, buildRequest(proto.OpReadAt, 2, openHeader.SessionID, proto.BuildReadBody(0, 1)))
	if err != nil {
		t.Fatalf("read with extension status: %v", err)
	}
	if header := mustParseGatewayHeader(t, readResp); header.StatusCode != 0x7E01 {
		t.Fatalf("unexpected passthrough status: %d", header.StatusCode)
	}
}

func TestGatewayCloseConnectionWithReasonPassesThroughProtocolError(t *testing.T) {
	t.Parallel()

	const diskID = "DISK000000000001"
	routes := &mappingRouteSource{
		entry: route.Entry{
			DiskID:       diskID,
			RouteTarget:  "storer://conn-41",
			ConnectionID: 41,
			Connected:    true,
		},
	}
	dataPlane := &mappingDataPlane{openSessionID: 155}

	handler, err := NewHandler(routes, dataPlane)
	if err != nil {
		t.Fatalf("new handler: %v", err)
	}
	state := handler.NewConnectionState(92)
	authID := handler.grants.Issue(state.ConnectionID(), diskID, time.Now().Add(time.Minute))

	openResp, err := handler.HandlePayload(state, buildRequest(proto.OpSessionOpen, 1, 0, proto.BuildSessionOpenRequestBody(authID)))
	if err != nil {
		t.Fatalf("open session: %v", err)
	}
	openHeader := mustParseGatewayHeader(t, openResp)
	if openHeader.StatusCode != proto.StatusOK {
		t.Fatalf("unexpected open status: %d", openHeader.StatusCode)
	}

	handler.CloseConnectionWithReason(state.ConnectionID(), proto.SessionCloseReasonProtocolError)
	if dataPlane.lastCloseSessionID != dataPlane.openSessionID {
		t.Fatalf("close connection did not release upstream session: %d", dataPlane.lastCloseSessionID)
	}
	if reason, err := proto.ParseSessionCloseNoticeBody(dataPlane.lastCloseBody); err != nil || reason != proto.SessionCloseReasonProtocolError {
		t.Fatalf("unexpected synthesized close body on protocol error: reason=%d err=%v", reason, err)
	}
}

func TestGatewaySessionDataChangedNoticeMapsRouteSessionToGatewaySession(t *testing.T) {
	t.Parallel()

	const diskID = "DISK000000000001"
	routes := &mappingRouteSource{
		entry: route.Entry{
			DiskID:       diskID,
			RouteTarget:  "storer://conn-51",
			ConnectionID: 51,
			Connected:    true,
		},
	}
	dataPlane := &mappingDataPlane{openSessionID: 166}

	handler, err := NewHandler(routes, dataPlane)
	if err != nil {
		t.Fatalf("new handler: %v", err)
	}
	notifier := &recordingSessionDataChangedNotifier{}
	handler.SetSessionDataChangedNotifier(notifier)

	state := handler.NewConnectionState(93)
	authID := handler.grants.Issue(state.ConnectionID(), diskID, time.Now().Add(time.Minute))
	openResp, err := handler.HandlePayload(state, buildRequest(proto.OpSessionOpen, 1, 0, proto.BuildSessionOpenRequestBody(authID)))
	if err != nil {
		t.Fatalf("open session: %v", err)
	}
	openHeader := mustParseGatewayHeader(t, openResp)
	if openHeader.StatusCode != proto.StatusOK {
		t.Fatalf("unexpected open status: %d", openHeader.StatusCode)
	}

	handler.NotifyRouteSessionDataChanged(routes.entry.ConnectionID, dataPlane.openSessionID)
	if notifier.count() != 1 {
		t.Fatalf("expected one data changed notice, got %d", notifier.count())
	}
	record := notifier.last()
	if record.sessionID != openHeader.SessionID {
		t.Fatalf("unexpected mapped gateway session id: %d", record.sessionID)
	}
	if record.clientConnectionID != state.ConnectionID() {
		t.Fatalf("unexpected client connection id: %d", record.clientConnectionID)
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
	readOut               []byte
	roundTripErr          error
	statusByOp            map[uint8]uint16
	lastDescribeSessionID uint64
	lastReadSessionID     uint64
	lastWriteSessionID    uint64
	lastWriteBody         []byte
	lastCloseSessionID    uint64
	lastCloseBody         []byte
	lastCloseConnectionID uint64
}

func (p *mappingDataPlane) Open(uint64, route.Entry) (uint64, error) {
	return p.openSessionID, p.openErr
}

func (p *mappingDataPlane) CloseConnection(connectionID uint64) {
	p.lastCloseConnectionID = connectionID
}

func (p *mappingDataPlane) RoundTrip(routeConnectionID uint64, sessionID uint64, opCode uint8, body []byte) (uint16, []byte, error) {
	if p.roundTripErr != nil {
		return 0, nil, p.roundTripErr
	}
	if status, ok := p.statusByOp[opCode]; ok {
		return status, nil, nil
	}

	switch opCode {
	case proto.OpSessionDescribe:
		p.lastDescribeSessionID = sessionID
		return proto.StatusOK, proto.BuildSessionDescribeResponseBody(
			p.describeOut.DiskSizeBytes,
			p.describeOut.MaxIOBytes,
			p.describeOut.ReadOnly,
			p.describeOut.BackendID,
		), nil
	case proto.OpReadAt:
		p.lastReadSessionID = sessionID
		return proto.StatusOK, bytes.Clone(p.readOut), nil
	case proto.OpWriteAt:
		p.lastWriteSessionID = sessionID
		p.lastWriteBody = bytes.Clone(body)
		return proto.StatusOK, nil, nil
	default:
		return proto.StatusUnsupportedOp, nil, nil
	}
}

func (p *mappingDataPlane) SendNotice(routeConnectionID uint64, sessionID uint64, opCode uint8, body []byte) error {
	if opCode == proto.OpSessionCloseNotice {
		p.lastCloseSessionID = sessionID
		p.lastCloseBody = bytes.Clone(body)
	}
	return nil
}

type recordingSessionCloseNotifier struct {
	mu      sync.Mutex
	records []sessionCloseRecord
}

type sessionCloseRecord struct {
	sessionID          uint64
	clientConnectionID uint64
	body               []byte
}

type recordingSessionDataChangedNotifier struct {
	mu      sync.Mutex
	records []sessionDataChangedRecord
}

type sessionDataChangedRecord struct {
	sessionID          uint64
	clientConnectionID uint64
}

func (n *recordingSessionCloseNotifier) NotifySessionClosed(sessionID uint64, clientConnectionID uint64, body []byte) {
	n.mu.Lock()
	n.records = append(n.records, sessionCloseRecord{
		sessionID:          sessionID,
		clientConnectionID: clientConnectionID,
		body:               bytes.Clone(body),
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

func (n *recordingSessionDataChangedNotifier) NotifySessionDataChanged(sessionID uint64, clientConnectionID uint64) {
	n.mu.Lock()
	n.records = append(n.records, sessionDataChangedRecord{
		sessionID:          sessionID,
		clientConnectionID: clientConnectionID,
	})
	n.mu.Unlock()
}

func (n *recordingSessionDataChangedNotifier) count() int {
	n.mu.Lock()
	defer n.mu.Unlock()
	return len(n.records)
}

func (n *recordingSessionDataChangedNotifier) last() sessionDataChangedRecord {
	n.mu.Lock()
	defer n.mu.Unlock()
	return n.records[len(n.records)-1]
}
