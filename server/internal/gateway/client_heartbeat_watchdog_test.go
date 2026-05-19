package gateway

import (
	"net"
	"testing"
	"time"

	"yumedisk/server/internal/proto"
)

func TestClientHeartbeatWatchdogTimesOutWithoutHeartbeat(t *testing.T) {
	t.Parallel()

	serverConn, clientConn := net.Pipe()
	defer serverConn.Close()
	defer clientConn.Close()

	watchdog := newClientHeartbeatWatchdog(100 * time.Millisecond)
	watchdog.Mark()
	errCh := watchdog.Start(serverConn)
	defer watchdog.Stop()

	select {
	case err := <-errCh:
		if err != errConnHeartbeatTimeout {
			t.Fatalf("unexpected watchdog error: %v", err)
		}
	case <-time.After(time.Second):
		t.Fatal("watchdog did not time out")
	}
}

func TestConnHeartbeatMarksClientWatchdog(t *testing.T) {
	t.Parallel()

	handler := &Handler{
		sessionOpener: newSessionOpener(nil, nil, newAuthGrantRegistry()),
		grants:        newAuthGrantRegistry(),
	}
	state := handler.NewConnectionState(17)
	watchdog := newClientHeartbeatWatchdog(time.Second)
	state.setHeartbeatWatchdog(watchdog)

	resp, err := handler.HandlePayload(state, buildRequest(proto.OpConnHeartbeat, 1, 0, nil))
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
