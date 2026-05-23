package gateway

import (
	"bytes"
	"net"
	"os"
	"path/filepath"
	"testing"
	"time"

	"yumedisk/server/internal/proto"
	"yumedisk/server/internal/session"
	filestorage "yumedisk/server/internal/storage/file"
)

func TestDataPlaneHandlerOpenReadWriteAndClose(t *testing.T) {
	t.Parallel()

	core := newLinkTestCore(t)
	handler := newDataPlaneHandler(17, core.SessionService(), nil)

	openResp, err := handler.HandlePayload(buildGatewayRequest(proto.OpSessionOpen, 1, 0, nil))
	if err != nil {
		t.Fatalf("open session: %v", err)
	}
	openHeader := mustParseGatewayHeader(t, openResp)
	if openHeader.StatusCode != proto.StatusOK {
		t.Fatalf("unexpected open status: %d", openHeader.StatusCode)
	}

	writePayload := append(proto.BuildReadWriteBody(4, 4), []byte("YUME")...)
	writeResp, err := handler.HandlePayload(buildGatewayRequest(proto.OpWriteAt, 2, openHeader.SessionID, writePayload))
	if err != nil {
		t.Fatalf("write: %v", err)
	}
	if header := mustParseGatewayHeader(t, writeResp); header.StatusCode != proto.StatusOK {
		t.Fatalf("unexpected write status: %d", header.StatusCode)
	}

	readResp, err := handler.HandlePayload(buildGatewayRequest(proto.OpReadAt, 3, openHeader.SessionID, proto.BuildReadBody(4, 4)))
	if err != nil {
		t.Fatalf("read: %v", err)
	}
	readHeader := mustParseGatewayHeader(t, readResp)
	if readHeader.StatusCode != proto.StatusOK {
		t.Fatalf("unexpected read status: %d", readHeader.StatusCode)
	}
	if !bytes.Equal(readResp[proto.HeaderSize:], []byte("YUME")) {
		t.Fatalf("unexpected read payload: %q", string(readResp[proto.HeaderSize:]))
	}

	closeResp, err := handler.HandlePayload(buildGatewayNotice(
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

	readAfterCloseResp, err := handler.HandlePayload(buildGatewayRequest(proto.OpReadAt, 5, openHeader.SessionID, proto.BuildReadBody(4, 4)))
	if err != nil {
		t.Fatalf("read after close: %v", err)
	}
	if header := mustParseGatewayHeader(t, readAfterCloseResp); header.StatusCode != proto.StatusSessionUnavailable {
		t.Fatalf("unexpected read-after-close status: %d", header.StatusCode)
	}
}

func TestDataPlaneHandlerRejectsBadOpenBodyAndAuthOps(t *testing.T) {
	t.Parallel()

	core := newLinkTestCore(t)
	handler := newDataPlaneHandler(18, core.SessionService(), nil)

	openResp, err := handler.HandlePayload(buildGatewayRequest(proto.OpSessionOpen, 1, 0, []byte("unexpected")))
	if err != nil {
		t.Fatalf("open with body: %v", err)
	}
	if header := mustParseGatewayHeader(t, openResp); header.StatusCode != proto.StatusBadBody {
		t.Fatalf("unexpected open-with-body status: %d", header.StatusCode)
	}

	authResp, err := handler.HandlePayload(buildGatewayRequest(proto.OpAuthStart, 2, 0, []byte(core.DiskID())))
	if err != nil {
		t.Fatalf("auth start: %v", err)
	}
	if header := mustParseGatewayHeader(t, authResp); header.StatusCode != proto.StatusUnsupportedOp {
		t.Fatalf("unexpected auth-op status: %d", header.StatusCode)
	}
}

func TestLinkHeartbeatWatchdogTimesOutWithoutHeartbeat(t *testing.T) {
	t.Parallel()

	serverConn, clientConn := net.Pipe()
	defer serverConn.Close()
	defer clientConn.Close()

	watchdog := newLinkHeartbeatWatchdog(100 * time.Millisecond)
	watchdog.Mark()
	errCh := watchdog.Start(serverConn)
	defer watchdog.Stop()

	select {
	case err := <-errCh:
		if err != errLinkHeartbeatTimeout {
			t.Fatalf("unexpected watchdog error: %v", err)
		}
	case <-time.After(time.Second):
		t.Fatal("watchdog did not time out")
	}
}

func TestDataPlaneHandlerMarksWatchdogOnLinkHeartbeat(t *testing.T) {
	t.Parallel()

	core := newLinkTestCore(t)
	watchdog := newLinkHeartbeatWatchdog(time.Second)
	handler := newDataPlaneHandler(19, core.SessionService(), watchdog)

	resp, err := handler.HandlePayload(buildGatewayRequest(
		proto.OpLinkHeartbeat,
		1,
		0,
		proto.BuildLinkHeartbeatBody(9),
	))
	if err != nil {
		t.Fatalf("handle heartbeat: %v", err)
	}
	if header := mustParseGatewayHeader(t, resp); header.StatusCode != proto.StatusOK {
		t.Fatalf("unexpected heartbeat status: %d", header.StatusCode)
	}
	if watchdog.deadlineUnixNano.Load() == 0 {
		t.Fatal("expected watchdog mark to be updated")
	}
}

func buildGatewayRequest(opCode uint8, requestID uint64, sessionID uint64, body []byte) []byte {
	payload := make([]byte, proto.HeaderSize+len(body))
	proto.EncodeHeader(proto.Header{
		ProtocolVersion: proto.ProtocolVersion,
		HeaderLen:       proto.HeaderSize,
		OpCode:          opCode,
		RequestID:       requestID,
		SessionID:       sessionID,
	}, payload)
	copy(payload[proto.HeaderSize:], body)
	return payload
}

func buildGatewayNotice(opCode uint8, sessionID uint64, body []byte) []byte {
	return proto.BuildNotice(opCode, sessionID, body)
}

func mustParseGatewayHeader(t *testing.T, payload []byte) proto.Header {
	t.Helper()

	header, err := proto.ParseHeader(payload)
	if err != nil {
		t.Fatalf("parse header: %v", err)
	}
	return header
}

func newLinkTestCore(t *testing.T) *linkTestCore {
	t.Helper()

	tempDir := t.TempDir()
	rawPath := filepath.Join(tempDir, "disk.raw")
	if err := os.WriteFile(rawPath, make([]byte, 4096), 0o644); err != nil {
		t.Fatalf("write raw file: %v", err)
	}

	storage, err := filestorage.Open(rawPath, false)
	if err != nil {
		t.Fatalf("open storage: %v", err)
	}
	t.Cleanup(func() { _ = storage.Close() })

	metadata := session.Metadata{
		DiskID:        "DISK000000000001",
		DiskSizeBytes: 4096,
		ReadOnly:      false,
		MaxIOBytes:    60 * 1024,
	}
	return &linkTestCore{
		sessions: session.NewService(session.NewManager(), storage, metadata),
		metadata: metadata,
	}
}

type linkTestCore struct {
	sessions *session.Service
	metadata session.Metadata
}

func (c *linkTestCore) DiskID() string {
	return c.metadata.DiskID
}

func (c *linkTestCore) SessionService() *session.Service {
	return c.sessions
}
