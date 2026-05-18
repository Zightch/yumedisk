package storer

import (
	"net"
	"testing"
	"time"

	"yumedisk/server/internal/proto"
)

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

	core := newTestCore(t)
	watchdog := newLinkHeartbeatWatchdog(time.Second)
	handler := newDataPlaneHandler(19, core.SessionService(), watchdog)

	resp, err := handler.HandlePayload(buildRequest(
		proto.OpLinkHeartbeat,
		1,
		0,
		proto.BuildLinkHeartbeatBody(9),
	))
	if err != nil {
		t.Fatalf("handle heartbeat: %v", err)
	}
	if header := mustParseHeader(t, resp); header.StatusCode != proto.StatusOK {
		t.Fatalf("unexpected heartbeat status: %d", header.StatusCode)
	}
	if watchdog.deadlineUnixNano.Load() == 0 {
		t.Fatal("expected watchdog mark to be updated")
	}
}
