package proto

import (
	"encoding/binary"
	"errors"
)

const (
	ProtocolVersion = 1
	HeaderSize      = 24
	FlagResponse    = 1 << 0
)

const (
	OpAuthStart   = 0x01
	OpAuthFinish  = 0x02
	OpSessionOpen = 0x03
	OpReadAt      = 0x10
	OpWriteAt     = 0x11
	OpPing        = 0x12
	OpClose       = 0x13
)

const (
	StatusOK              = 0x0000
	StatusProtocolVersion = 0x1001
	StatusBadHeader       = 0x1002
	StatusBadBody         = 0x1003
	StatusInvalidRequest  = 0x1004
	StatusUnsupportedOp   = 0x1005
)

var (
	ErrPayloadTooSmall   = errors.New("payload smaller than header")
	ErrProtocolVersion   = errors.New("unsupported protocol version")
	ErrHeaderLength      = errors.New("unsupported header length")
	ErrReservedNonZero   = errors.New("reserved field must be zero")
	ErrResponseBitOn     = errors.New("request header has response bit set")
	ErrRequestStatusNon0 = errors.New("request status_code must be zero")
	ErrRequestIDZero     = errors.New("request_id must be non-zero")
)

type Header struct {
	ProtocolVersion uint8
	HeaderLen       uint8
	OpCode          uint8
	Flags           uint8
	StatusCode      uint16
	Reserved        uint16
	RequestID       uint64
	SessionID       uint64
}

func ParseHeader(payload []byte) (Header, error) {
	if len(payload) < HeaderSize {
		return Header{}, ErrPayloadTooSmall
	}

	return Header{
		ProtocolVersion: payload[0],
		HeaderLen:       payload[1],
		OpCode:          payload[2],
		Flags:           payload[3],
		StatusCode:      binary.BigEndian.Uint16(payload[4:6]),
		Reserved:        binary.BigEndian.Uint16(payload[6:8]),
		RequestID:       binary.BigEndian.Uint64(payload[8:16]),
		SessionID:       binary.BigEndian.Uint64(payload[16:24]),
	}, nil
}

func ValidateRequestHeader(h Header) error {
	if h.ProtocolVersion != ProtocolVersion {
		return ErrProtocolVersion
	}
	if h.HeaderLen != HeaderSize {
		return ErrHeaderLength
	}
	if h.Flags&FlagResponse != 0 {
		return ErrResponseBitOn
	}
	if h.Flags&^FlagResponse != 0 {
		return ErrReservedNonZero
	}
	if h.StatusCode != 0 {
		return ErrRequestStatusNon0
	}
	if h.Reserved != 0 {
		return ErrReservedNonZero
	}
	if h.RequestID == 0 {
		return ErrRequestIDZero
	}
	return nil
}

func EncodeHeader(header Header, payload []byte) {
	payload[0] = header.ProtocolVersion
	payload[1] = header.HeaderLen
	payload[2] = header.OpCode
	payload[3] = header.Flags
	binary.BigEndian.PutUint16(payload[4:6], header.StatusCode)
	binary.BigEndian.PutUint16(payload[6:8], header.Reserved)
	binary.BigEndian.PutUint64(payload[8:16], header.RequestID)
	binary.BigEndian.PutUint64(payload[16:24], header.SessionID)
}

func BuildErrorResponse(req Header, status uint16) []byte {
	payload := make([]byte, HeaderSize)
	EncodeHeader(Header{
		ProtocolVersion: ProtocolVersion,
		HeaderLen:       HeaderSize,
		OpCode:          req.OpCode,
		Flags:           FlagResponse,
		StatusCode:      status,
		Reserved:        0,
		RequestID:       req.RequestID,
		SessionID:       req.SessionID,
	}, payload)
	return payload
}
