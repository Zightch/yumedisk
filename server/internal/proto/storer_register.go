package proto

import (
	"encoding/binary"
	"errors"
)

const (
	StorerRegisterHeaderSize = 2
	StorerRegisterDiskIDSize = 16
	StorerRegisterFlagsRO    = 1 << 0
	StorerRegisterFixedSize  = StorerRegisterHeaderSize + StorerRegisterDiskIDSize + AuthProofSize + 8 + 4 + 4 + 2 + 2
)

var ErrStorerRegisterBody = errors.New("storer register body invalid")

type StorerRegisterRequest struct {
	GatewayToken      string
	DiskID            string
	AuthVerifier      [64]byte
	DiskSizeBytes     uint64
	ReadOnly          bool
	MaxIOBytes        uint32
	SessionTTLSeconds uint32
}

func BuildStorerRegisterRequestBody(req StorerRegisterRequest) []byte {
	tokenBytes := []byte(req.GatewayToken)
	body := make([]byte, StorerRegisterHeaderSize+len(tokenBytes)+StorerRegisterDiskIDSize+AuthProofSize+8+4+4+2+2)
	binary.BigEndian.PutUint16(body[0:2], uint16(len(tokenBytes)))
	offset := StorerRegisterHeaderSize
	copy(body[offset:offset+len(tokenBytes)], tokenBytes)
	offset += len(tokenBytes)
	copy(body[offset:offset+StorerRegisterDiskIDSize], []byte(req.DiskID))
	offset += StorerRegisterDiskIDSize
	copy(body[offset:offset+AuthProofSize], req.AuthVerifier[:])
	offset += AuthProofSize
	binary.BigEndian.PutUint64(body[offset:offset+8], req.DiskSizeBytes)
	offset += 8
	binary.BigEndian.PutUint32(body[offset:offset+4], req.MaxIOBytes)
	offset += 4
	binary.BigEndian.PutUint32(body[offset:offset+4], req.SessionTTLSeconds)
	offset += 4
	var flags uint16
	if req.ReadOnly {
		flags = StorerRegisterFlagsRO
	}
	binary.BigEndian.PutUint16(body[offset:offset+2], flags)
	offset += 2
	binary.BigEndian.PutUint16(body[offset:offset+2], 0)
	return body
}

func ParseStorerRegisterRequestBody(body []byte) (StorerRegisterRequest, error) {
	if len(body) < StorerRegisterFixedSize {
		return StorerRegisterRequest{}, ErrStorerRegisterBody
	}
	tokenLen := int(binary.BigEndian.Uint16(body[0:2]))
	if tokenLen <= 0 {
		return StorerRegisterRequest{}, ErrStorerRegisterBody
	}
	expectedLen := StorerRegisterHeaderSize + tokenLen + StorerRegisterDiskIDSize + AuthProofSize + 8 + 4 + 4 + 2 + 2
	if len(body) != expectedLen {
		return StorerRegisterRequest{}, ErrStorerRegisterBody
	}

	offset := StorerRegisterHeaderSize
	token := string(body[offset : offset+tokenLen])
	offset += tokenLen

	diskIDBytes := body[offset : offset+StorerRegisterDiskIDSize]
	if !isAlphaNumericASCII(diskIDBytes) {
		return StorerRegisterRequest{}, ErrStorerRegisterBody
	}
	diskID := string(diskIDBytes)
	offset += StorerRegisterDiskIDSize

	var authVerifier [64]byte
	copy(authVerifier[:], body[offset:offset+AuthProofSize])
	offset += AuthProofSize

	diskSize := binary.BigEndian.Uint64(body[offset : offset+8])
	offset += 8
	maxIO := binary.BigEndian.Uint32(body[offset : offset+4])
	offset += 4
	ttl := binary.BigEndian.Uint32(body[offset : offset+4])
	offset += 4
	flags := binary.BigEndian.Uint16(body[offset : offset+2])
	offset += 2
	reserved := binary.BigEndian.Uint16(body[offset : offset+2])
	if reserved != 0 {
		return StorerRegisterRequest{}, ErrStorerRegisterBody
	}

	return StorerRegisterRequest{
		GatewayToken:      token,
		DiskID:            diskID,
		AuthVerifier:      authVerifier,
		DiskSizeBytes:     diskSize,
		ReadOnly:          flags&StorerRegisterFlagsRO != 0,
		MaxIOBytes:        maxIO,
		SessionTTLSeconds: ttl,
	}, nil
}
