package proto

import (
	"encoding/binary"
	"errors"
)

const (
	SessionOpenRequestSize  = 16
	SessionOpenResponseSize = 20
	SessionFlagReadOnly     = 1 << 0
	PingBodySize            = 8
	SessionCloseNoticeSize  = 2
	ReadWriteHeaderSize     = 12
)

var ErrSessionBody = errors.New("session body invalid")

const (
	SessionCloseReasonStorerLinkLost    = uint16(1)
	SessionCloseReasonGatewayShutdown   = uint16(2)
	SessionCloseReasonUpstreamHeartbeat = uint16(3)
	SessionCloseReasonNormalClose       = uint16(5)
	SessionCloseReasonProtocolError     = uint16(6)
)

func ParseSessionOpenRequestBody(body []byte) (string, error) {
	if len(body) != SessionOpenRequestSize {
		return "", ErrSessionBody
	}
	if !isAlphaNumericASCII(body) {
		return "", ErrSessionBody
	}
	return string(body), nil
}

func BuildSessionOpenResponseBody(diskSize uint64, maxIOBytes uint32, ttlSeconds uint32, readOnly bool) []byte {
	body := make([]byte, SessionOpenResponseSize)
	binary.BigEndian.PutUint64(body[0:8], diskSize)
	binary.BigEndian.PutUint32(body[8:12], maxIOBytes)
	binary.BigEndian.PutUint32(body[12:16], ttlSeconds)
	if readOnly {
		binary.BigEndian.PutUint16(body[16:18], SessionFlagReadOnly)
	}
	binary.BigEndian.PutUint16(body[18:20], 0)
	return body
}

func ParseSessionOpenResponseBody(body []byte) (diskSize uint64, maxIOBytes uint32, ttlSeconds uint32, readOnly bool, err error) {
	if len(body) != SessionOpenResponseSize {
		return 0, 0, 0, false, ErrSessionBody
	}
	diskSize = binary.BigEndian.Uint64(body[0:8])
	maxIOBytes = binary.BigEndian.Uint32(body[8:12])
	ttlSeconds = binary.BigEndian.Uint32(body[12:16])
	flags := binary.BigEndian.Uint16(body[16:18])
	readOnly = flags&SessionFlagReadOnly != 0
	return diskSize, maxIOBytes, ttlSeconds, readOnly, nil
}

func ParsePingRequestBody(body []byte) (uint64, error) {
	if len(body) != PingBodySize {
		return 0, ErrSessionBody
	}
	return binary.BigEndian.Uint64(body), nil
}

func BuildPingResponseBody(nonce uint64) []byte {
	body := make([]byte, PingBodySize)
	binary.BigEndian.PutUint64(body, nonce)
	return body
}

func BuildSessionCloseNoticeBody(reason uint16) []byte {
	body := make([]byte, SessionCloseNoticeSize)
	binary.BigEndian.PutUint16(body[0:2], reason)
	return body
}

func ParseSessionCloseNoticeBody(body []byte) (uint16, error) {
	if len(body) != SessionCloseNoticeSize {
		return 0, ErrSessionBody
	}
	return binary.BigEndian.Uint16(body[0:2]), nil
}

func ParseReadWriteBody(body []byte) (offset uint64, length uint32, data []byte, err error) {
	if len(body) < ReadWriteHeaderSize {
		return 0, 0, nil, ErrSessionBody
	}

	offset = binary.BigEndian.Uint64(body[0:8])
	length = binary.BigEndian.Uint32(body[8:12])
	if uint64(len(body)) != uint64(ReadWriteHeaderSize)+uint64(length) {
		return 0, 0, nil, ErrSessionBody
	}

	data = make([]byte, length)
	copy(data, body[12:])
	return offset, length, data, nil
}

func ParseReadBody(body []byte) (offset uint64, length uint32, err error) {
	if len(body) != ReadWriteHeaderSize {
		return 0, 0, ErrSessionBody
	}
	offset = binary.BigEndian.Uint64(body[0:8])
	length = binary.BigEndian.Uint32(body[8:12])
	return offset, length, nil
}

func BuildReadBody(offset uint64, length uint32) []byte {
	body := make([]byte, ReadWriteHeaderSize)
	binary.BigEndian.PutUint64(body[0:8], offset)
	binary.BigEndian.PutUint32(body[8:12], length)
	return body
}

func BuildReadWriteBody(offset uint64, length uint32) []byte {
	body := make([]byte, ReadWriteHeaderSize)
	binary.BigEndian.PutUint64(body[0:8], offset)
	binary.BigEndian.PutUint32(body[8:12], length)
	return body
}
