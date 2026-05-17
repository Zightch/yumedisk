package gateway

import (
	"context"
	"net"
	"strconv"
	"testing"
	"time"

	"yumedisk/server/internal/config"
)

func TestGatewayRuntimeListensOnClientAndStorerPorts(t *testing.T) {
	t.Parallel()

	cfg := config.GatewayConfig{
		Client: config.GatewayClientConfig{
			ListenAddr: reserveGatewayLocalAddr(t),
		},
		Storer: config.GatewayStorerConfig{
			ListenAddr:   reserveGatewayLocalAddr(t),
			GatewayToken: "dev-gateway-token",
		},
	}

	runtime, err := NewRuntime(cfg)
	if err != nil {
		t.Fatalf("new gateway runtime: %v", err)
	}

	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()

	done := make(chan error, 1)
	go func() {
		done <- runtime.Run(ctx)
	}()

	waitForGatewayTCP(t, cfg.Client.ListenAddr)
	waitForGatewayTCP(t, cfg.Storer.ListenAddr)

	clientConn, err := net.Dial("tcp", cfg.Client.ListenAddr)
	if err != nil {
		t.Fatalf("dial client listener: %v", err)
	}
	_ = clientConn.Close()

	storerConn, err := net.Dial("tcp", cfg.Storer.ListenAddr)
	if err != nil {
		t.Fatalf("dial storer listener: %v", err)
	}
	_ = storerConn.Close()

	cancel()
	select {
	case err := <-done:
		if err != nil {
			t.Fatalf("gateway runtime exited with error: %v", err)
		}
	case <-time.After(3 * time.Second):
		t.Fatal("gateway runtime did not stop in time")
	}
}

func reserveGatewayLocalAddr(t *testing.T) string {
	t.Helper()

	listener, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		t.Fatalf("reserve local addr: %v", err)
	}
	defer listener.Close()

	tcpAddr, ok := listener.Addr().(*net.TCPAddr)
	if !ok {
		t.Fatalf("unexpected listener addr type: %T", listener.Addr())
	}
	return net.JoinHostPort("127.0.0.1", strconv.Itoa(tcpAddr.Port))
}

func waitForGatewayTCP(t *testing.T, addr string) {
	t.Helper()

	deadline := time.Now().Add(3 * time.Second)
	for time.Now().Before(deadline) {
		conn, err := net.DialTimeout("tcp", addr, 100*time.Millisecond)
		if err == nil {
			_ = conn.Close()
			return
		}
		time.Sleep(50 * time.Millisecond)
	}
	t.Fatalf("gateway runtime did not start listening on %s", addr)
}
