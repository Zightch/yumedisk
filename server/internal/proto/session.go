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
)

var ErrSessionBody = errors.New("session body invalid")

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
