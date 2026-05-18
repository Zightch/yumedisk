package bootstrap

import (
	"encoding/binary"
	"errors"
	"fmt"
	"io"
)

const (
	HelloVersion = 1
	HeaderSize   = 12
	MaxBodySize  = 64 * 1024
)

const (
	messageTypeRequest  = 1
	messageTypeResponse = 2
)

const (
	StatusOK              = 0
	StatusProtocolVersion = 1
	StatusInvalidRequest  = 2
)

var (
	headerMagic = [4]byte{'Y', 'D', 'H', 'L'}

	ErrInvalidHello     = errors.New("invalid hello")
	ErrProtocolVersion  = errors.New("unsupported hello version")
	ErrPayloadTooLarge  = errors.New("hello payload too large")
	ErrUnexpectedStatus = errors.New("unexpected hello status")
)

type Response struct {
	Version            uint8
	Status             uint16
	ServerCapabilities []byte
}

func WriteHelloRequest(w io.Writer) error {
	return writeMessage(w, HelloVersion, messageTypeRequest, 0, nil)
}

func ReadHelloRequest(r io.Reader) (uint8, error) {
	version, messageType, status, body, err := readMessage(r)
	if err != nil {
		return 0, err
	}
	if messageType != messageTypeRequest || status != 0 || len(body) != 0 {
		return 0, ErrInvalidHello
	}
	return version, nil
}

func WriteHelloResponse(w io.Writer, response Response) error {
	return writeMessage(
		w,
		response.Version,
		messageTypeResponse,
		response.Status,
		response.ServerCapabilities,
	)
}

func ReadHelloResponse(r io.Reader) (Response, error) {
	version, messageType, status, body, err := readMessage(r)
	if err != nil {
		return Response{}, err
	}
	if messageType != messageTypeResponse {
		return Response{}, ErrInvalidHello
	}
	return Response{
		Version:            version,
		Status:             status,
		ServerCapabilities: body,
	}, nil
}

func AcceptClient(rw io.ReadWriter) error {
	version, err := ReadHelloRequest(rw)
	if err != nil {
		return err
	}
	if version != HelloVersion {
		if writeErr := WriteHelloResponse(rw, Response{
			Version: HelloVersion,
			Status:  StatusProtocolVersion,
		}); writeErr != nil {
			return fmt.Errorf("write hello response: %w", writeErr)
		}
		return ErrProtocolVersion
	}
	if err := WriteHelloResponse(rw, Response{
		Version:            HelloVersion,
		Status:             StatusOK,
		ServerCapabilities: nil,
	}); err != nil {
		return fmt.Errorf("write hello response: %w", err)
	}
	return nil
}

func ConnectClient(rw io.ReadWriter) (Response, error) {
	if err := WriteHelloRequest(rw); err != nil {
		return Response{}, fmt.Errorf("write hello request: %w", err)
	}
	response, err := ReadHelloResponse(rw)
	if err != nil {
		return Response{}, fmt.Errorf("read hello response: %w", err)
	}
	if response.Version != HelloVersion {
		return Response{}, ErrProtocolVersion
	}
	if response.Status != StatusOK {
		switch response.Status {
		case StatusProtocolVersion:
			return Response{}, ErrProtocolVersion
		default:
			return Response{}, ErrUnexpectedStatus
		}
	}
	return response, nil
}

func writeMessage(w io.Writer, version uint8, messageType uint8, status uint16, body []byte) error {
	if len(body) > MaxBodySize {
		return ErrPayloadTooLarge
	}
	header := make([]byte, HeaderSize)
	copy(header[0:4], headerMagic[:])
	header[4] = version
	header[5] = messageType
	binary.BigEndian.PutUint16(header[6:8], status)
	binary.BigEndian.PutUint32(header[8:12], uint32(len(body)))
	if err := writeAll(w, header); err != nil {
		return err
	}
	if len(body) == 0 {
		return nil
	}
	return writeAll(w, body)
}

func readMessage(r io.Reader) (uint8, uint8, uint16, []byte, error) {
	header := make([]byte, HeaderSize)
	if _, err := io.ReadFull(r, header); err != nil {
		return 0, 0, 0, nil, err
	}
	if header[0] != headerMagic[0] ||
		header[1] != headerMagic[1] ||
		header[2] != headerMagic[2] ||
		header[3] != headerMagic[3] {
		return 0, 0, 0, nil, ErrInvalidHello
	}

	bodyLen := binary.BigEndian.Uint32(header[8:12])
	if bodyLen > MaxBodySize {
		return 0, 0, 0, nil, ErrPayloadTooLarge
	}
	body := make([]byte, int(bodyLen))
	if _, err := io.ReadFull(r, body); err != nil {
		return 0, 0, 0, nil, err
	}
	return header[4], header[5], binary.BigEndian.Uint16(header[6:8]), body, nil
}

func writeAll(w io.Writer, data []byte) error {
	for len(data) > 0 {
		n, err := w.Write(data)
		if err != nil {
			return err
		}
		data = data[n:]
	}
	return nil
}
