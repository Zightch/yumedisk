package proto

import (
	"encoding/binary"
	"errors"
)

const (
	ProtocolVersion = 1
	HeaderSize      = 24
	FlagResponse    = 1 << 0
	FlagNotice      = 1 << 1
)

const (
	OpAuthStart          = 0x01
	OpAuthFinish         = 0x02
	OpSessionOpen        = 0x03
	OpSessionDescribe    = 0x04
	OpSessionCloseNotice = 0x05
	OpReadAt             = 0x10
	OpWriteAt            = 0x11
	OpConnHeartbeat      = 0x12
	OpClose              = 0x13
	OpStorerRegister     = 0x20
	OpLinkHeartbeat      = 0x21
)

const (
	StatusOK                   = 0x0000
	StatusProtocolVersion      = 0x1001
	StatusBadHeader            = 0x1002
	StatusBadBody              = 0x1003
	StatusInvalidRequest       = 0x1004
	StatusUnsupportedOp        = 0x1005
	StatusAuthFailed           = 0x1101
	StatusAuthExpired          = 0x1102
	StatusAuthChallengeInvalid = 0x1103
	StatusAuthIDInvalid        = 0x1104
	StatusAuthIDExpired        = 0x1105
	StatusSessionUnavailable   = 0x1201
	StatusSessionBusy          = 0x1202
	StatusIOOutOfRange         = 0x1301
	StatusIOLarge              = 0x1302
	StatusIOReadOnly           = 0x1303
	StatusIOFailed             = 0x1304
)

var (
	ErrPayloadTooSmall   = errors.New("payload smaller than header")
	ErrProtocolVersion   = errors.New("unsupported protocol version")
	ErrHeaderLength      = errors.New("unsupported header length")
	ErrReservedNonZero   = errors.New("reserved field must be zero")
	ErrInvalidFlags      = errors.New("header flags invalid")
	ErrRequestStatusNon0 = errors.New("request status_code must be zero")
	ErrRequestIDZero     = errors.New("request_id must be non-zero")
	ErrNoticeRequestID   = errors.New("notice request_id must be zero")
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
	if err := validateCommonHeader(h); err != nil {
		return err
	}
	if h.Flags != 0 {
		return ErrInvalidFlags
	}
	if h.StatusCode != 0 {
		return ErrRequestStatusNon0
	}
	if h.RequestID == 0 {
		return ErrRequestIDZero
	}
	return nil
}

func ValidateResponseHeader(h Header) error {
	if err := validateCommonHeader(h); err != nil {
		return err
	}
	if h.Flags != FlagResponse {
		return ErrInvalidFlags
	}
	if h.RequestID == 0 {
		return ErrRequestIDZero
	}
	return nil
}

func ValidateNoticeHeader(h Header) error {
	if err := validateCommonHeader(h); err != nil {
		return err
	}
	if h.Flags != FlagNotice {
		return ErrInvalidFlags
	}
	if h.RequestID != 0 {
		return ErrNoticeRequestID
	}
	return nil
}

func validateCommonHeader(h Header) error {
	if h.ProtocolVersion != ProtocolVersion {
		return ErrProtocolVersion
	}
	if h.HeaderLen != HeaderSize {
		return ErrHeaderLength
	}
	if h.Reserved != 0 {
		return ErrReservedNonZero
	}
	if h.Flags&^uint8(FlagResponse|FlagNotice) != 0 {
		return ErrInvalidFlags
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
	return BuildResponse(req, status, nil)
}

func BuildSuccessResponse(req Header, body []byte) []byte {
	return BuildResponse(req, StatusOK, body)
}

func BuildResponse(req Header, status uint16, body []byte) []byte {
	return BuildResponseWithSessionID(req, status, req.SessionID, body)
}

func BuildResponseWithSessionID(req Header, status uint16, sessionID uint64, body []byte) []byte {
	payload := make([]byte, HeaderSize+len(body))
	EncodeHeader(Header{
		ProtocolVersion: ProtocolVersion,
		HeaderLen:       HeaderSize,
		OpCode:          req.OpCode,
		Flags:           FlagResponse,
		StatusCode:      status,
		Reserved:        0,
		RequestID:       req.RequestID,
		SessionID:       sessionID,
	}, payload)
	copy(payload[HeaderSize:], body)
	return payload
}

func BuildNotice(opCode uint8, sessionID uint64, body []byte) []byte {
	payload := make([]byte, HeaderSize+len(body))
	EncodeHeader(Header{
		ProtocolVersion: ProtocolVersion,
		HeaderLen:       HeaderSize,
		OpCode:          opCode,
		Flags:           FlagNotice,
		StatusCode:      StatusOK,
		Reserved:        0,
		RequestID:       0,
		SessionID:       sessionID,
	}, payload)
	copy(payload[HeaderSize:], body)
	return payload
}
