package transport

import (
	"encoding/binary"
	"errors"
	"fmt"
	"io"
	"net"
	"sync"
)

const (
	MaxPayloadSize = 65536
	MinPayloadSize = 1
)

var (
	ErrBufferTooSmall    = errors.New("frame buffer too small")
	ErrPayloadOutOfRange = errors.New("payload size out of range")
	ErrConnectionClosed  = errors.New("transport connection closed")
)

type Handler interface {
	HandlePayload(payload []byte) ([]byte, error)
}

type Runtime struct {
	conn    net.Conn
	handler Handler

	writeMu sync.Mutex
}

func NewRuntime(conn net.Conn, handler Handler) *Runtime {
	return &Runtime{
		conn:    conn,
		handler: handler,
	}
}

func (r *Runtime) Run() error {
	buffer := make([]byte, MaxPayloadSize)

	for {
		payload, err := ReadFrameInto(r.conn, buffer)
		if err != nil {
			if errors.Is(err, io.EOF) || errors.Is(err, net.ErrClosed) {
				return nil
			}
			return err
		}

		response, err := r.handler.HandlePayload(payload)
		if err != nil {
			return err
		}
		if response == nil {
			continue
		}

		if err := r.WritePayload(response); err != nil {
			if errors.Is(err, net.ErrClosed) {
				return nil
			}
			return err
		}
	}
}

func (r *Runtime) Close() error {
	if r.conn == nil {
		return nil
	}
	err := r.conn.Close()
	r.conn = nil
	return err
}

func (r *Runtime) WritePayload(payload []byte) error {
	r.writeMu.Lock()
	defer r.writeMu.Unlock()

	if r.conn == nil {
		return ErrConnectionClosed
	}
	return WriteFrame(r.conn, payload)
}

func ReadFrameInto(r io.Reader, buffer []byte) ([]byte, error) {
	var header [2]byte
	if _, err := io.ReadFull(r, header[:]); err != nil {
		return nil, err
	}

	payloadSize := int(binary.BigEndian.Uint16(header[:])) + 1
	if payloadSize < MinPayloadSize || payloadSize > MaxPayloadSize {
		return nil, fmt.Errorf("%w: %d", ErrPayloadOutOfRange, payloadSize)
	}
	if len(buffer) < payloadSize {
		return nil, ErrBufferTooSmall
	}

	payload := buffer[:payloadSize]
	if _, err := io.ReadFull(r, payload); err != nil {
		return nil, err
	}
	return payload, nil
}

func WriteFrame(w io.Writer, payload []byte) error {
	if len(payload) < MinPayloadSize || len(payload) > MaxPayloadSize {
		return fmt.Errorf("%w: %d", ErrPayloadOutOfRange, len(payload))
	}

	var header [2]byte
	binary.BigEndian.PutUint16(header[:], uint16(len(payload)-1))

	if _, err := w.Write(header[:]); err != nil {
		return err
	}
	if _, err := w.Write(payload); err != nil {
		return err
	}
	return nil
}
