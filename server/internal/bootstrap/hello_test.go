package bootstrap

import (
	"errors"
	"net"
	"testing"
)

func TestHelloExchangeSucceedsWithEmptyCapabilities(t *testing.T) {
	t.Parallel()

	serverConn, clientConn := net.Pipe()
	defer serverConn.Close()
	defer clientConn.Close()

	done := make(chan error, 1)
	go func() {
		done <- AcceptClient(serverConn)
	}()

	response, err := ConnectClient(clientConn)
	if err != nil {
		t.Fatalf("connect client hello: %v", err)
	}
	if len(response.ServerCapabilities) != 0 {
		t.Fatalf("expected empty server capabilities, got %d bytes", len(response.ServerCapabilities))
	}
	if err := <-done; err != nil {
		t.Fatalf("accept client hello: %v", err)
	}
}

func TestAcceptClientRejectsUnsupportedVersion(t *testing.T) {
	t.Parallel()

	serverConn, clientConn := net.Pipe()
	defer serverConn.Close()
	defer clientConn.Close()

	done := make(chan error, 1)
	go func() {
		done <- AcceptClient(serverConn)
	}()

	if err := writeMessage(clientConn, HelloVersion+1, messageTypeRequest, 0, nil); err != nil {
		t.Fatalf("write hello request: %v", err)
	}
	response, err := ReadHelloResponse(clientConn)
	if err != nil {
		t.Fatalf("read hello response: %v", err)
	}
	if response.Status != StatusProtocolVersion {
		t.Fatalf("unexpected hello status: %d", response.Status)
	}
	if err := <-done; !errors.Is(err, ErrProtocolVersion) {
		t.Fatalf("expected protocol version error, got: %v", err)
	}
}
