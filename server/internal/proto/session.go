package proto

import (
	"encoding/binary"
	"errors"
)

const (
	SessionOpenRequestSize      = 8
	SessionDescribeResponseSize = 16
	SessionFlagReadOnly         = 1 << 0
	LinkHeartbeatBodySize       = 8
	SessionCloseNoticeSize      = 2
	ReadWriteHeaderSize         = 12
)

var ErrSessionBody = errors.New("session body invalid")

const (
	SessionCloseReasonRouteLost               = uint16(1)
	SessionCloseReasonGatewayShutdown         = uint16(2)
	SessionCloseReasonUpstreamSessionClosed   = uint16(3)
	SessionCloseReasonClientConnectionReplace = uint16(4)
	SessionCloseReasonNormalCloseMirror       = uint16(5)
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

func BuildSessionDescribeResponseBody(diskSize uint64, maxIOBytes uint32, readOnly bool) []byte {
	body := make([]byte, SessionDescribeResponseSize)
	binary.BigEndian.PutUint64(body[0:8], diskSize)
	binary.BigEndian.PutUint32(body[8:12], maxIOBytes)
	if readOnly {
		binary.BigEndian.PutUint16(body[12:14], SessionFlagReadOnly)
	}
	binary.BigEndian.PutUint16(body[14:16], 0)
	return body
}

func ParseSessionDescribeResponseBody(body []byte) (diskSize uint64, maxIOBytes uint32, readOnly bool, err error) {
	if len(body) != SessionDescribeResponseSize {
		return 0, 0, false, ErrSessionBody
	}
	diskSize = binary.BigEndian.Uint64(body[0:8])
	maxIOBytes = binary.BigEndian.Uint32(body[8:12])
	flags := binary.BigEndian.Uint16(body[12:14])
	reserved := binary.BigEndian.Uint16(body[14:16])
	if reserved != 0 {
		return 0, 0, false, ErrSessionBody
	}
	readOnly = flags&SessionFlagReadOnly != 0
	return diskSize, maxIOBytes, readOnly, nil
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
