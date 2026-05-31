package proto

import (
	"encoding/binary"
	"errors"
)

const (
	SessionOpenRequestSize      = 8
	SessionDescribeResponseSize = 28
	SessionFlagReadOnly         = 1 << 0
	LinkHeartbeatBodySize       = 8
	SessionCloseNoticeSize      = 2
	ReadBodySize                = 12
	WriteBodyHeaderSize         = 13
	ReadResponseHeaderSize      = 1
	CompressRaw                 = 0
)

var ErrSessionBody = errors.New("session body invalid")

const (
	SessionCloseReasonRouteLost               = uint16(1)
	SessionCloseReasonGatewayShutdown         = uint16(2)
	SessionCloseReasonUpstreamSessionClosed   = uint16(3)
	SessionCloseReasonClientConnectionReplace = uint16(4)
	SessionCloseReasonNormalClose             = uint16(5)
	SessionCloseReasonProtocolError           = uint16(6)
)

func ParseSessionOpenRequestBody(body []byte) (uint64, error) {
	if len(body) != SessionOpenRequestSize {
		return 0, ErrSessionBody
	}
	authID := binary.BigEndian.Uint64(body)
	if authID == 0 {
		return 0, ErrSessionBody
	}
	return authID, nil
}

func BuildSessionOpenRequestBody(authID uint64) []byte {
	body := make([]byte, SessionOpenRequestSize)
	binary.BigEndian.PutUint64(body, authID)
	return body
}

func BuildSessionDescribeResponseBody(diskSize uint64, readOnly bool, backendID [16]byte) []byte {
	body := make([]byte, SessionDescribeResponseSize)
	binary.BigEndian.PutUint64(body[0:8], diskSize)
	if readOnly {
		binary.BigEndian.PutUint16(body[8:10], SessionFlagReadOnly)
	}
	binary.BigEndian.PutUint16(body[10:12], 0)
	copy(body[12:28], backendID[:])
	return body
}

func ParseSessionDescribeResponseBody(body []byte) (diskSize uint64, readOnly bool, backendID [16]byte, err error) {
	if len(body) != SessionDescribeResponseSize {
		return 0, false, [16]byte{}, ErrSessionBody
	}
	diskSize = binary.BigEndian.Uint64(body[0:8])
	flags := binary.BigEndian.Uint16(body[8:10])
	reserved := binary.BigEndian.Uint16(body[10:12])
	if reserved != 0 {
		return 0, false, [16]byte{}, ErrSessionBody
	}
	readOnly = flags&SessionFlagReadOnly != 0
	copy(backendID[:], body[12:28])
	return diskSize, readOnly, backendID, nil
}

func ParseLinkHeartbeatBody(body []byte) (uint64, error) {
	if len(body) != LinkHeartbeatBodySize {
		return 0, ErrSessionBody
	}
	return binary.BigEndian.Uint64(body), nil
}

func BuildLinkHeartbeatBody(nonce uint64) []byte {
	body := make([]byte, LinkHeartbeatBodySize)
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
	if len(body) < WriteBodyHeaderSize {
		return 0, 0, nil, ErrSessionBody
	}

	offset = binary.BigEndian.Uint64(body[0:8])
	length = binary.BigEndian.Uint32(body[8:12])
	if body[12] != CompressRaw {
		return 0, 0, nil, ErrSessionBody
	}
	if uint64(len(body)) != uint64(WriteBodyHeaderSize)+uint64(length) {
		return 0, 0, nil, ErrSessionBody
	}

	data = make([]byte, length)
	copy(data, body[13:])
	return offset, length, data, nil
}

func ParseReadBody(body []byte) (offset uint64, length uint32, err error) {
	if len(body) != ReadBodySize {
		return 0, 0, ErrSessionBody
	}
	offset = binary.BigEndian.Uint64(body[0:8])
	length = binary.BigEndian.Uint32(body[8:12])
	return offset, length, nil
}

func BuildReadBody(offset uint64, length uint32) []byte {
	body := make([]byte, ReadBodySize)
	binary.BigEndian.PutUint64(body[0:8], offset)
	binary.BigEndian.PutUint32(body[8:12], length)
	return body
}

func BuildReadResponseBody(data []byte) []byte {
	body := make([]byte, ReadResponseHeaderSize+len(data))
	body[0] = CompressRaw
	copy(body[ReadResponseHeaderSize:], data)
	return body
}

func ParseReadResponseBody(body []byte, expectedLength uint32) ([]byte, error) {
	if len(body) < ReadResponseHeaderSize {
		return nil, ErrSessionBody
	}
	if body[0] != CompressRaw {
		return nil, ErrSessionBody
	}
	if len(body) != ReadResponseHeaderSize+int(expectedLength) {
		return nil, ErrSessionBody
	}

	data := make([]byte, int(expectedLength))
	copy(data, body[ReadResponseHeaderSize:])
	return data, nil
}

func BuildReadWriteBody(offset uint64, length uint32) []byte {
	body := make([]byte, WriteBodyHeaderSize)
	binary.BigEndian.PutUint64(body[0:8], offset)
	binary.BigEndian.PutUint32(body[8:12], length)
	body[12] = CompressRaw
	return body
}
