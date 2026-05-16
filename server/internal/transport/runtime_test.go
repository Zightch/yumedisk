package transport

import (
	"bytes"
	"errors"
	"net"
	"sync"
	"testing"
	"time"
)

type stubHandler struct {
	mu        sync.Mutex
	payloads  [][]byte
	responses [][]byte
	err       error
}

func (h *stubHandler) HandlePayload(payload []byte) ([]byte, error) {
	h.mu.Lock()
	defer h.mu.Unlock()

	copied := make([]byte, len(payload))
	copy(copied, payload)
	h.payloads = append(h.payloads, copied)

	if h.err != nil {
		return nil, h.err
	}
	if len(h.responses) == 0 {
		return nil, nil
	}

	response := h.responses[0]
	h.responses = h.responses[1:]
	return response, nil
}

func TestReadFrameIntoAndWriteFrame(t *testing.T) {
	t.Parallel()

	var stream bytes.Buffer
	payload := []byte("hello")
	if err := WriteFrame(&stream, payload); err != nil {
		t.Fatalf("WriteFrame returned error: %v", err)
	}

	buffer := make([]byte, MaxPayloadSize)
	got, err := ReadFrameInto(&stream, buffer)
	if err != nil {
		t.Fatalf("ReadFrameInto returned error: %v", err)
	}
	if string(got) != "hello" {
		t.Fatalf("payload mismatch: got %q", string(got))
	}
}

func TestReadFrameIntoRejectsSmallBuffer(t *testing.T) {
	t.Parallel()

	var stream bytes.Buffer
	if err := WriteFrame(&stream, []byte("hello")); err != nil {
		t.Fatalf("WriteFrame returned error: %v", err)
	}

	_, err := ReadFrameInto(&stream, make([]byte, 4))
	if !errors.Is(err, ErrBufferTooSmall) {
		t.Fatalf("expected ErrBufferTooSmall, got %v", err)
	}
}

func TestRuntimeRunsAcceptedConnectionLoop(t *testing.T) {
	t.Parallel()

	serverConn, clientConn := net.Pipe()
	defer clientConn.Close()

	handler := &stubHandler{
		responses: [][]byte{
			[]byte("resp-1"),
			[]byte("resp-2"),
		},
	}
	runtime := NewRuntime(serverConn, handler)

	done := make(chan error, 1)
	go func() {
		done <- runtime.Run()
	}()

	if err := WriteFrame(clientConn, []byte("req-1")); err != nil {
		t.Fatalf("write request 1: %v", err)
	}
	buffer := make([]byte, MaxPayloadSize)
	resp1, err := ReadFrameInto(clientConn, buffer)
	if err != nil {
		t.Fatalf("read response 1: %v", err)
	}
	resp1Copy := append([]byte(nil), resp1...)

	if err := WriteFrame(clientConn, []byte("req-2")); err != nil {
		t.Fatalf("write request 2: %v", err)
	}
	resp2, err := ReadFrameInto(clientConn, buffer)
	if err != nil {
		t.Fatalf("read response 2: %v", err)
	}
	resp2Copy := append([]byte(nil), resp2...)

	if string(resp1Copy) != "resp-1" {
		t.Fatalf("response 1 mismatch: %q", string(resp1Copy))
	}
	if string(resp2Copy) != "resp-2" {
		t.Fatalf("response 2 mismatch: %q", string(resp2Copy))
	}

	if err := clientConn.Close(); err != nil {
		t.Fatalf("close client conn: %v", err)
	}

	select {
	case err := <-done:
		if err != nil {
			t.Fatalf("runtime run returned error: %v", err)
		}
	case <-time.After(2 * time.Second):
		t.Fatal("runtime did not stop after connection close")
	}

	handler.mu.Lock()
	defer handler.mu.Unlock()
	if len(handler.payloads) != 2 {
		t.Fatalf("handler payload count mismatch: %d", len(handler.payloads))
	}
}
