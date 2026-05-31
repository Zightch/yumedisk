package storer

import (
	"bytes"
	"fmt"
	"net"
	"testing"

	"yumedisk/server/internal/proto"
	"yumedisk/server/internal/transport"
)

func TestDataPlaneRoundTripPassesThroughOpaqueRequestStatusAndResponse(t *testing.T) {
	t.Parallel()

	registry := NewRegistry()
	serverConn, clientConn := net.Pipe()
	defer serverConn.Close()
	defer clientConn.Close()
	conn := registry.AttachConnection(7, serverConn)

	requestBody := []byte{0x10, 0x20, 0x30, 0x40, 0x50}
	responseBody := []byte{0xAA, 0xBB, 0xCC, 0xDD}
	errCh := make(chan error, 1)
	pumpErrCh := make(chan error, 1)
	go func() {
		buffer := make([]byte, transport.MaxPayloadSize)
		payload, err := transport.ReadFrameInto(serverConn, buffer)
		if err != nil {
			conn.closePending()
			pumpErrCh <- fmt.Errorf("read inbound route response: %w", err)
			return
		}
		header, err := proto.ParseHeader(payload)
		if err != nil {
			conn.closePending()
			pumpErrCh <- fmt.Errorf("parse inbound route response: %w", err)
			return
		}
		conn.handleResponse(header, payload)
		pumpErrCh <- nil
	}()
	go func() {
		buffer := make([]byte, transport.MaxPayloadSize)
		req, err := transport.ReadFrameInto(clientConn, buffer)
		if err != nil {
			_ = clientConn.Close()
			errCh <- fmt.Errorf("read route request: %w", err)
			return
		}
		header, err := proto.ParseHeader(req)
		if err != nil {
			_ = clientConn.Close()
			errCh <- fmt.Errorf("parse route request: %w", err)
			return
		}
		if err := proto.ValidateRequestHeader(header); err != nil {
			_ = clientConn.Close()
			errCh <- fmt.Errorf("validate route request: %w", err)
			return
		}
		if header.OpCode != proto.OpWriteAt {
			_ = clientConn.Close()
			errCh <- fmt.Errorf("unexpected route request op: %d", header.OpCode)
			return
		}
		if header.SessionID != 99 {
			_ = clientConn.Close()
			errCh <- fmt.Errorf("unexpected route session id: %d", header.SessionID)
			return
		}
		if !bytes.Equal(req[proto.HeaderSize:], requestBody) {
			_ = clientConn.Close()
			errCh <- fmt.Errorf("unexpected route request body: got=%v want=%v", req[proto.HeaderSize:], requestBody)
			return
		}

		resp := make([]byte, proto.HeaderSize+len(responseBody))
		proto.EncodeHeader(proto.Header{
			ProtocolVersion: proto.ProtocolVersion,
			HeaderLen:       proto.HeaderSize,
			OpCode:          proto.OpWriteAt,
			Flags:           proto.FlagResponse,
			StatusCode:      0x7E01,
			RequestID:       header.RequestID,
			SessionID:       header.SessionID,
		}, resp)
		copy(resp[proto.HeaderSize:], responseBody)
		if err := transport.WriteFrame(clientConn, resp); err != nil {
			errCh <- fmt.Errorf("write route response: %w", err)
			return
		}
		errCh <- nil
	}()

	status, body, err := registry.DataPlane().RoundTrip(7, 99, proto.OpWriteAt, requestBody)
	if err != nil {
		t.Fatalf("round trip: %v", err)
	}
	if status != 0x7E01 {
		t.Fatalf("unexpected passthrough status: %d", status)
	}
	if !bytes.Equal(body, responseBody) {
		t.Fatalf("unexpected passthrough response body: got=%v want=%v", body, responseBody)
	}
	if err := <-pumpErrCh; err != nil {
		t.Fatal(err)
	}
	if err := <-errCh; err != nil {
		t.Fatal(err)
	}
}

func TestDataPlaneSendNoticePassesThroughOpaqueBody(t *testing.T) {
	t.Parallel()

	registry := NewRegistry()
	serverConn, clientConn := net.Pipe()
	defer serverConn.Close()
	defer clientConn.Close()
	registry.AttachConnection(8, serverConn)

	noticeBody := []byte{0xFE, 0xED, 0xFA}
	errCh := make(chan error, 1)
	go func() {
		buffer := make([]byte, transport.MaxPayloadSize)
		payload, err := transport.ReadFrameInto(clientConn, buffer)
		if err != nil {
			errCh <- fmt.Errorf("read route notice: %w", err)
			return
		}
		header, err := proto.ParseHeader(payload)
		if err != nil {
			errCh <- fmt.Errorf("parse route notice: %w", err)
			return
		}
		if err := proto.ValidateNoticeHeader(header); err != nil {
			errCh <- fmt.Errorf("validate route notice: %w", err)
			return
		}
		if header.OpCode != proto.OpSessionCloseNotice {
			errCh <- fmt.Errorf("unexpected route notice op: %d", header.OpCode)
			return
		}
		if header.SessionID != 123 {
			errCh <- fmt.Errorf("unexpected route notice session id: %d", header.SessionID)
			return
		}
		if !bytes.Equal(payload[proto.HeaderSize:], noticeBody) {
			errCh <- fmt.Errorf("unexpected route notice body: got=%v want=%v", payload[proto.HeaderSize:], noticeBody)
			return
		}
		errCh <- nil
	}()

	if err := registry.DataPlane().SendNotice(8, 123, proto.OpSessionCloseNotice, noticeBody); err != nil {
		t.Fatalf("send notice: %v", err)
	}
	if err := <-errCh; err != nil {
		t.Fatal(err)
	}
}
