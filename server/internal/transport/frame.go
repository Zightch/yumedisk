package transport

import (
	"encoding/binary"
	"errors"
	"fmt"
	"io"
)

const (
	MaxPayloadSize = 65536
	MinPayloadSize = 1
)

var ErrBufferTooSmall = errors.New("frame buffer too small")

func ReadFrameInto(r io.Reader, buffer []byte) ([]byte, error) {
	var header [2]byte
	if _, err := io.ReadFull(r, header[:]); err != nil {
		return nil, err
	}

	payloadSize := int(binary.BigEndian.Uint16(header[:])) + 1
	if payloadSize < MinPayloadSize || payloadSize > MaxPayloadSize {
		return nil, fmt.Errorf("invalid payload size %d", payloadSize)
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
		return fmt.Errorf("payload size must be within %d..%d bytes", MinPayloadSize, MaxPayloadSize)
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
