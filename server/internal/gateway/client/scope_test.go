package client

import (
	"bytes"
	"os"
	"path/filepath"
	"sync"
	"testing"
	"time"

	"yumedisk/server/internal/proto"
	"yumedisk/server/internal/route"
	"yumedisk/server/internal/session"
	filestorage "yumedisk/server/internal/storage/file"
)

const (
	scopeTestRWDiskID        = "DISK_SCOPE_RW001"
	scopeTestRODiskID        = "DISK_SCOPE_RO001"
	scopeTestRWRouteConnID   = uint64(101)
	scopeTestRORouteConnID   = uint64(202)
	scopeTestDiskSizeBytes   = uint64(4096)
	scopeTestPendingConnIDRW = uint64(9001)
	scopeTestPendingConnIDRO = uint64(9002)
)

func TestGatewayCloseConnectionOnlyClosesThatClientROSession(t *testing.T) {
	t.Parallel()

	backend := newScopeTestBackend(t)
	handler, err := NewHandler(backend, backend)
	if err != nil {
		t.Fatalf("new handler: %v", err)
	}
	backend.seed(t, 64, []byte("ROOK"))

	stateOne := handler.NewConnectionState(11)
	stateTwo := handler.NewConnectionState(22)
	roSessionOne := openScopeSession(t, handler, stateOne, scopeTestRODiskID, 1)
	roSessionTwo := openScopeSession(t, handler, stateTwo, scopeTestRODiskID, 1)

	handler.CloseConnection(stateOne.ConnectionID())

	closeCalls := backend.closeCalls()
	if len(closeCalls) != 1 {
		t.Fatalf("unexpected close call count: %d", len(closeCalls))
	}
	if closeCalls[0].routeConnectionID != scopeTestRORouteConnID {
		t.Fatalf("unexpected closed route id: %d", closeCalls[0].routeConnectionID)
	}
	if closeCalls[0].sessionID != backend.upstreamSessionID(t, scopeTestRORouteConnID, stateOne.ConnectionID()) {
		t.Fatalf("unexpected closed session id: %d", closeCalls[0].sessionID)
	}

	resp, err := handler.HandlePayload(stateOne, buildRequest(proto.OpReadAt, 2, roSessionOne, proto.BuildReadBody(64, 4)))
	if err != nil {
		t.Fatalf("read after close: %v", err)
	}
	if header := mustParseGatewayHeader(t, resp); header.StatusCode != proto.StatusSessionUnavailable {
		t.Fatalf("unexpected read-after-close status: %d", header.StatusCode)
	}

	resp, err = handler.HandlePayload(stateTwo, buildRequest(proto.OpReadAt, 2, roSessionTwo, proto.BuildReadBody(64, 4)))
	if err != nil {
		t.Fatalf("peer read after other connection close: %v", err)
	}
	if header := mustParseGatewayHeader(t, resp); header.StatusCode != proto.StatusOK {
		t.Fatalf("unexpected peer read status: %d", header.StatusCode)
	}
	if !bytes.Equal(resp[proto.HeaderSize:], proto.BuildReadResponseBody([]byte("ROOK"))) {
		t.Fatalf("unexpected peer read payload: %q", string(resp[proto.HeaderSize:]))
	}
}

func TestGatewayWriterConnectionCloseReleasesWriterAndLeavesROSessionsLive(t *testing.T) {
	t.Parallel()

	backend := newScopeTestBackend(t)
	handler, err := NewHandler(backend, backend)
	if err != nil {
		t.Fatalf("new handler: %v", err)
	}

	writerStateOne := handler.NewConnectionState(31)
	roState := handler.NewConnectionState(32)
	rwSessionOne := openScopeSession(t, handler, writerStateOne, scopeTestRWDiskID, 1)
	roSession := openScopeSession(t, handler, roState, scopeTestRODiskID, 1)

	writePayload := append(proto.BuildReadWriteBody(128, 4), []byte("W001")...)
	resp, err := handler.HandlePayload(writerStateOne, buildRequest(proto.OpWriteAt, 2, rwSessionOne, writePayload))
	if err != nil {
		t.Fatalf("writer write: %v", err)
	}
	if header := mustParseGatewayHeader(t, resp); header.StatusCode != proto.StatusOK {
		t.Fatalf("unexpected first writer status: %d", header.StatusCode)
	}

	handler.CloseConnection(writerStateOne.ConnectionID())

	readResp, err := handler.HandlePayload(roState, buildRequest(proto.OpReadAt, 2, roSession, proto.BuildReadBody(128, 4)))
	if err != nil {
		t.Fatalf("ro read after writer close: %v", err)
	}
	if header := mustParseGatewayHeader(t, readResp); header.StatusCode != proto.StatusOK {
		t.Fatalf("unexpected ro read status: %d", header.StatusCode)
	}
	if !bytes.Equal(readResp[proto.HeaderSize:], proto.BuildReadResponseBody([]byte("W001"))) {
		t.Fatalf("unexpected ro read payload: %q", string(readResp[proto.HeaderSize:]))
	}

	writerStateTwo := handler.NewConnectionState(33)
	rwSessionTwo := openScopeSession(t, handler, writerStateTwo, scopeTestRWDiskID, 1)
	writePayload = append(proto.BuildReadWriteBody(132, 4), []byte("W002")...)
	resp, err = handler.HandlePayload(writerStateTwo, buildRequest(proto.OpWriteAt, 2, rwSessionTwo, writePayload))
	if err != nil {
		t.Fatalf("second writer write: %v", err)
	}
	if header := mustParseGatewayHeader(t, resp); header.StatusCode != proto.StatusOK {
		t.Fatalf("unexpected second writer status: %d", header.StatusCode)
	}

	readResp, err = handler.HandlePayload(roState, buildRequest(proto.OpReadAt, 3, roSession, proto.BuildReadBody(132, 4)))
	if err != nil {
		t.Fatalf("ro read after second writer: %v", err)
	}
	if header := mustParseGatewayHeader(t, readResp); header.StatusCode != proto.StatusOK {
		t.Fatalf("unexpected ro read-after-second-writer status: %d", header.StatusCode)
	}
	if !bytes.Equal(readResp[proto.HeaderSize:], proto.BuildReadResponseBody([]byte("W002"))) {
		t.Fatalf("unexpected ro read-after-second-writer payload: %q", string(readResp[proto.HeaderSize:]))
	}
}

func TestGatewayRORouteDisconnectOnlyAffectsROSessions(t *testing.T) {
	t.Parallel()

	backend := newScopeTestBackend(t)
	handler, err := NewHandler(backend, backend)
	if err != nil {
		t.Fatalf("new handler: %v", err)
	}
	notifier := &recordingSessionCloseNotifier{}
	handler.SetSessionCloseNotifier(notifier)

	rwState := handler.NewConnectionState(41)
	roStateOne := handler.NewConnectionState(42)
	roStateTwo := handler.NewConnectionState(43)
	rwSession := openScopeSession(t, handler, rwState, scopeTestRWDiskID, 1)
	roSessionOne := openScopeSession(t, handler, roStateOne, scopeTestRODiskID, 1)
	roSessionTwo := openScopeSession(t, handler, roStateTwo, scopeTestRODiskID, 1)
	backend.seed(t, 196, []byte("ROUT"))

	pendingRW := handler.grants.Issue(scopeTestPendingConnIDRW, scopeTestRWDiskID, time.Now().Add(time.Minute))
	pendingRO := handler.grants.Issue(scopeTestPendingConnIDRO, scopeTestRODiskID, time.Now().Add(time.Minute))

	backend.disconnectRoute(scopeTestRORouteConnID)
	handler.CloseRouteConnection(scopeTestRORouteConnID, []string{scopeTestRODiskID})

	if notifier.count() != 2 {
		t.Fatalf("unexpected notifier count: %d", notifier.count())
	}
	closedIDs := map[uint64]struct{}{
		roSessionOne: {},
		roSessionTwo: {},
	}
	for _, record := range notifier.snapshot() {
		reason, err := proto.ParseSessionCloseNoticeBody(record.body)
		if err != nil {
			t.Fatalf("parse route-lost close body: %v", err)
		}
		if reason != proto.SessionCloseReasonRouteLost {
			t.Fatalf("unexpected close reason: %d", reason)
		}
		if _, ok := closedIDs[record.sessionID]; !ok {
			t.Fatalf("unexpected closed session id: %d", record.sessionID)
		}
	}

	if _, status, ok := handler.grants.Lookup(pendingRO, scopeTestPendingConnIDRO); ok || status != proto.StatusAuthIDInvalid {
		t.Fatalf("expected ro grant revoked, ok=%v status=%d", ok, status)
	}
	if _, status, ok := handler.grants.Lookup(pendingRW, scopeTestPendingConnIDRW); !ok || status != proto.StatusOK {
		t.Fatalf("expected rw grant to remain, ok=%v status=%d", ok, status)
	}

	resp, err := handler.HandlePayload(rwState, buildRequest(proto.OpReadAt, 2, rwSession, proto.BuildReadBody(196, 4)))
	if err != nil {
		t.Fatalf("rw read after ro route disconnect: %v", err)
	}
	if header := mustParseGatewayHeader(t, resp); header.StatusCode != proto.StatusOK {
		t.Fatalf("unexpected rw read status: %d", header.StatusCode)
	}

	resp, err = handler.HandlePayload(roStateOne, buildRequest(proto.OpReadAt, 2, roSessionOne, proto.BuildReadBody(196, 4)))
	if err != nil {
		t.Fatalf("ro read after route disconnect: %v", err)
	}
	if header := mustParseGatewayHeader(t, resp); header.StatusCode != proto.StatusSessionUnavailable {
		t.Fatalf("unexpected ro read-after-disconnect status: %d", header.StatusCode)
	}
}

func TestGatewayRWRouteDisconnectOnlyAffectsRWSessions(t *testing.T) {
	t.Parallel()

	backend := newScopeTestBackend(t)
	handler, err := NewHandler(backend, backend)
	if err != nil {
		t.Fatalf("new handler: %v", err)
	}
	notifier := &recordingSessionCloseNotifier{}
	handler.SetSessionCloseNotifier(notifier)
	backend.seed(t, 228, []byte("KEEP"))

	rwState := handler.NewConnectionState(51)
	roState := handler.NewConnectionState(52)
	rwSession := openScopeSession(t, handler, rwState, scopeTestRWDiskID, 1)
	roSession := openScopeSession(t, handler, roState, scopeTestRODiskID, 1)

	pendingRW := handler.grants.Issue(scopeTestPendingConnIDRW, scopeTestRWDiskID, time.Now().Add(time.Minute))
	pendingRO := handler.grants.Issue(scopeTestPendingConnIDRO, scopeTestRODiskID, time.Now().Add(time.Minute))

	backend.disconnectRoute(scopeTestRWRouteConnID)
	handler.CloseRouteConnection(scopeTestRWRouteConnID, []string{scopeTestRWDiskID})

	if notifier.count() != 1 {
		t.Fatalf("unexpected notifier count: %d", notifier.count())
	}
	record := notifier.last()
	if record.sessionID != rwSession {
		t.Fatalf("unexpected closed rw session id: %d", record.sessionID)
	}
	reason, err := proto.ParseSessionCloseNoticeBody(record.body)
	if err != nil {
		t.Fatalf("parse route-lost close body: %v", err)
	}
	if reason != proto.SessionCloseReasonRouteLost {
		t.Fatalf("unexpected close reason: %d", reason)
	}

	if _, status, ok := handler.grants.Lookup(pendingRW, scopeTestPendingConnIDRW); ok || status != proto.StatusAuthIDInvalid {
		t.Fatalf("expected rw grant revoked, ok=%v status=%d", ok, status)
	}
	if _, status, ok := handler.grants.Lookup(pendingRO, scopeTestPendingConnIDRO); !ok || status != proto.StatusOK {
		t.Fatalf("expected ro grant to remain, ok=%v status=%d", ok, status)
	}

	resp, err := handler.HandlePayload(roState, buildRequest(proto.OpReadAt, 2, roSession, proto.BuildReadBody(228, 4)))
	if err != nil {
		t.Fatalf("ro read after rw route disconnect: %v", err)
	}
	if header := mustParseGatewayHeader(t, resp); header.StatusCode != proto.StatusOK {
		t.Fatalf("unexpected ro read status: %d", header.StatusCode)
	}

	resp, err = handler.HandlePayload(rwState, buildRequest(proto.OpReadAt, 2, rwSession, proto.BuildReadBody(228, 4)))
	if err != nil {
		t.Fatalf("rw read after route disconnect: %v", err)
	}
	if header := mustParseGatewayHeader(t, resp); header.StatusCode != proto.StatusSessionUnavailable {
		t.Fatalf("unexpected rw read-after-disconnect status: %d", header.StatusCode)
	}
}

func TestGatewaySessionUnavailableDoesNotEscalateToRouteFailure(t *testing.T) {
	t.Parallel()

	backend := newScopeTestBackend(t)
	handler, err := NewHandler(backend, backend)
	if err != nil {
		t.Fatalf("new handler: %v", err)
	}
	backend.seed(t, 260, []byte("LIVE"))

	stateOne := handler.NewConnectionState(61)
	stateTwo := handler.NewConnectionState(62)
	sessionOne := openScopeSession(t, handler, stateOne, scopeTestRODiskID, 1)
	sessionTwo := openScopeSession(t, handler, stateTwo, scopeTestRODiskID, 1)

	backend.closeUpstream(t, scopeTestRORouteConnID, stateOne.ConnectionID())

	resp, err := handler.HandlePayload(stateOne, buildRequest(proto.OpReadAt, 2, sessionOne, proto.BuildReadBody(260, 4)))
	if err != nil {
		t.Fatalf("read failed session: %v", err)
	}
	if header := mustParseGatewayHeader(t, resp); header.StatusCode != proto.StatusSessionUnavailable {
		t.Fatalf("unexpected failed-session status: %d", header.StatusCode)
	}

	resp, err = handler.HandlePayload(stateTwo, buildRequest(proto.OpReadAt, 2, sessionTwo, proto.BuildReadBody(260, 4)))
	if err != nil {
		t.Fatalf("read peer session: %v", err)
	}
	if header := mustParseGatewayHeader(t, resp); header.StatusCode != proto.StatusOK {
		t.Fatalf("unexpected peer session status: %d", header.StatusCode)
	}
	if !bytes.Equal(resp[proto.HeaderSize:], proto.BuildReadResponseBody([]byte("LIVE"))) {
		t.Fatalf("unexpected peer read payload: %q", string(resp[proto.HeaderSize:]))
	}

	stateThree := handler.NewConnectionState(63)
	sessionThree := openScopeSession(t, handler, stateThree, scopeTestRODiskID, 1)
	resp, err = handler.HandlePayload(stateThree, buildRequest(proto.OpReadAt, 2, sessionThree, proto.BuildReadBody(260, 4)))
	if err != nil {
		t.Fatalf("read new peer session: %v", err)
	}
	if header := mustParseGatewayHeader(t, resp); header.StatusCode != proto.StatusOK {
		t.Fatalf("unexpected new peer session status: %d", header.StatusCode)
	}
}

func TestGatewayOpenRejectDoesNotClearExistingROSession(t *testing.T) {
	t.Parallel()

	backend := newScopeTestBackend(t)
	handler, err := NewHandler(backend, backend)
	if err != nil {
		t.Fatalf("new handler: %v", err)
	}
	backend.seed(t, 292, []byte("SAFE"))

	writerState := handler.NewConnectionState(71)
	roState := handler.NewConnectionState(72)
	secondWriterState := handler.NewConnectionState(73)
	_ = openScopeSession(t, handler, writerState, scopeTestRWDiskID, 1)
	roSession := openScopeSession(t, handler, roState, scopeTestRODiskID, 1)

	authID := handler.grants.Issue(secondWriterState.ConnectionID(), scopeTestRWDiskID, time.Now().Add(time.Minute))
	resp, err := handler.HandlePayload(secondWriterState, buildRequest(proto.OpSessionOpen, 1, 0, proto.BuildSessionOpenRequestBody(authID)))
	if err != nil {
		t.Fatalf("second writer open: %v", err)
	}
	if header := mustParseGatewayHeader(t, resp); header.StatusCode != proto.StatusSessionOpenRejected {
		t.Fatalf("unexpected second writer open status: %d", header.StatusCode)
	}

	resp, err = handler.HandlePayload(roState, buildRequest(proto.OpReadAt, 2, roSession, proto.BuildReadBody(292, 4)))
	if err != nil {
		t.Fatalf("ro read after writer reject: %v", err)
	}
	if header := mustParseGatewayHeader(t, resp); header.StatusCode != proto.StatusOK {
		t.Fatalf("unexpected ro read status: %d", header.StatusCode)
	}
	if !bytes.Equal(resp[proto.HeaderSize:], proto.BuildReadResponseBody([]byte("SAFE"))) {
		t.Fatalf("unexpected ro read payload: %q", string(resp[proto.HeaderSize:]))
	}
	if len(backend.closeCalls()) != 0 {
		t.Fatalf("unexpected upstream close calls after open reject: %+v", backend.closeCalls())
	}
}

func openScopeSession(t *testing.T, handler *Handler, state *ConnectionState, diskID string, requestID uint64) uint64 {
	t.Helper()

	authID := handler.grants.Issue(state.ConnectionID(), diskID, time.Now().Add(time.Minute))
	resp, err := handler.HandlePayload(state, buildRequest(proto.OpSessionOpen, requestID, 0, proto.BuildSessionOpenRequestBody(authID)))
	if err != nil {
		t.Fatalf("open session for disk %s: %v", diskID, err)
	}
	header := mustParseGatewayHeader(t, resp)
	if header.StatusCode != proto.StatusOK {
		t.Fatalf("unexpected open status for disk %s: %d", diskID, header.StatusCode)
	}
	return header.SessionID
}

type scopeTestBackend struct {
	routes   *route.Registry
	storage  *filestorage.Backend
	services map[uint64]*session.Service

	mu            sync.Mutex
	upstreamByKey map[scopeSessionKey]uint64
	closeLog      []scopeCloseCall
}

type scopeSessionKey struct {
	routeConnectionID uint64
	clientConnection  uint64
}

type scopeCloseCall struct {
	routeConnectionID uint64
	sessionID         uint64
}

func newScopeTestBackend(t *testing.T) *scopeTestBackend {
	t.Helper()

	tempDir := t.TempDir()
	rawPath := filepath.Join(tempDir, "disk.raw")
	if err := os.WriteFile(rawPath, make([]byte, scopeTestDiskSizeBytes), 0o644); err != nil {
		t.Fatalf("write raw file: %v", err)
	}

	storage, err := filestorage.Open(rawPath, false)
	if err != nil {
		t.Fatalf("open storage: %v", err)
	}
	t.Cleanup(func() { _ = storage.Close() })

	routes := route.NewRegistry()
	rwEntry := route.Entry{
		DiskID:       scopeTestRWDiskID,
		RouteTarget:  "storer://scope-rw",
		ConnectionID: scopeTestRWRouteConnID,
		Connected:    true,
	}
	roEntry := route.Entry{
		DiskID:       scopeTestRODiskID,
		RouteTarget:  "storer://scope-ro",
		ConnectionID: scopeTestRORouteConnID,
		Connected:    true,
	}
	if err := routes.Register(rwEntry); err != nil {
		t.Fatalf("register rw route: %v", err)
	}
	if err := routes.Register(roEntry); err != nil {
		t.Fatalf("register ro route: %v", err)
	}

	return &scopeTestBackend{
		routes:  routes,
		storage: storage,
		services: map[uint64]*session.Service{
			scopeTestRWRouteConnID: session.NewService(session.NewExclusiveManager(), storage, session.Metadata{
				DiskID:        scopeTestRWDiskID,
				DiskSizeBytes: scopeTestDiskSizeBytes,
				ReadOnly:      false,
			}),
			scopeTestRORouteConnID: session.NewService(session.NewSharedManager(), storage, session.Metadata{
				DiskID:        scopeTestRODiskID,
				DiskSizeBytes: scopeTestDiskSizeBytes,
				ReadOnly:      true,
			}),
		},
		upstreamByKey: make(map[scopeSessionKey]uint64),
	}
}

func (b *scopeTestBackend) LookupRoute(diskID string) (route.Entry, bool) {
	return b.routes.LookupRoute(diskID)
}

func (b *scopeTestBackend) Open(connectionID uint64, entry route.Entry) (uint64, error) {
	service := b.services[entry.ConnectionID]
	desc, err := service.Open(connectionID)
	if err != nil {
		return 0, err
	}

	b.mu.Lock()
	b.upstreamByKey[scopeSessionKey{
		routeConnectionID: entry.ConnectionID,
		clientConnection:  connectionID,
	}] = desc.ID
	b.mu.Unlock()
	return desc.ID, nil
}

func (b *scopeTestBackend) CloseConnection(uint64) {}

func (b *scopeTestBackend) RoundTrip(routeConnectionID uint64, sessionID uint64, opCode uint8, body []byte) (uint16, []byte, error) {
	service := b.services[routeConnectionID]

	switch opCode {
	case proto.OpSessionDescribe:
		if sessionID == 0 {
			return proto.StatusBadHeader, nil, nil
		}
		if len(body) != 0 {
			return proto.StatusBadBody, nil, nil
		}
		metadata, err := service.Describe(sessionID)
		if err != nil {
			return mapScopeSessionErrorStatus(err), nil, nil
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
		data, err := service.Read(sessionID, offset, length)
		if err != nil {
			return mapScopeSessionErrorStatus(err), nil, nil
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
		if err := service.Write(sessionID, offset, data); err != nil {
			return mapScopeSessionErrorStatus(err), nil, nil
		}
		return proto.StatusOK, nil, nil
	default:
		return proto.StatusUnsupportedOp, nil, nil
	}
}

func (b *scopeTestBackend) SendNotice(routeConnectionID uint64, sessionID uint64, opCode uint8, body []byte) error {
	if opCode != proto.OpSessionCloseNotice {
		return nil
	}
	service := b.services[routeConnectionID]
	b.mu.Lock()
	b.closeLog = append(b.closeLog, scopeCloseCall{
		routeConnectionID: routeConnectionID,
		sessionID:         sessionID,
	})
	b.mu.Unlock()
	service.Close(sessionID)
	return nil
}

func (b *scopeTestBackend) seed(t *testing.T, offset uint64, data []byte) {
	t.Helper()
	if err := b.storage.WriteAt(offset, data); err != nil {
		t.Fatalf("seed storage: %v", err)
	}
}

func (b *scopeTestBackend) disconnectRoute(routeConnectionID uint64) {
	b.routes.DisconnectConnection(routeConnectionID)
}

func (b *scopeTestBackend) upstreamSessionID(t *testing.T, routeConnectionID uint64, clientConnectionID uint64) uint64 {
	t.Helper()
	b.mu.Lock()
	defer b.mu.Unlock()
	sessionID, ok := b.upstreamByKey[scopeSessionKey{
		routeConnectionID: routeConnectionID,
		clientConnection:  clientConnectionID,
	}]
	if !ok {
		t.Fatalf("missing upstream session for route=%d conn=%d", routeConnectionID, clientConnectionID)
	}
	return sessionID
}

func (b *scopeTestBackend) closeUpstream(t *testing.T, routeConnectionID uint64, clientConnectionID uint64) {
	t.Helper()
	sessionID := b.upstreamSessionID(t, routeConnectionID, clientConnectionID)
	b.services[routeConnectionID].Close(sessionID)
}

func (b *scopeTestBackend) closeCalls() []scopeCloseCall {
	b.mu.Lock()
	defer b.mu.Unlock()
	out := make([]scopeCloseCall, len(b.closeLog))
	copy(out, b.closeLog)
	return out
}

func mapScopeSessionErrorStatus(err error) uint16 {
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

func (n *recordingSessionCloseNotifier) snapshot() []sessionCloseRecord {
	n.mu.Lock()
	defer n.mu.Unlock()
	out := make([]sessionCloseRecord, len(n.records))
	copy(out, n.records)
	return out
}
