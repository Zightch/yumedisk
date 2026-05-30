package proto

import (
	"encoding/binary"
	"errors"
)

const (
	StorerRegisterHeaderSize = 2
	StorerRegisterDiskIDSize = 16
	StorerRegisterFixedSize  = StorerRegisterHeaderSize + StorerRegisterDiskIDSize + AuthProofSize
)

var ErrStorerRegisterBody = errors.New("storer register body invalid")

type StorerRegisterRequest struct {
	GatewayToken string
	DiskID       string
	AuthVerifier [64]byte
}

func BuildStorerRegisterRequestBody(req StorerRegisterRequest) []byte {
	tokenBytes := []byte(req.GatewayToken)
	body := make([]byte, StorerRegisterHeaderSize+len(tokenBytes)+StorerRegisterDiskIDSize+AuthProofSize)
	binary.BigEndian.PutUint16(body[0:2], uint16(len(tokenBytes)))
	offset := StorerRegisterHeaderSize
	copy(body[offset:offset+len(tokenBytes)], tokenBytes)
	offset += len(tokenBytes)
	copy(body[offset:offset+StorerRegisterDiskIDSize], []byte(req.DiskID))
	offset += StorerRegisterDiskIDSize
	copy(body[offset:offset+AuthProofSize], req.AuthVerifier[:])
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
	expectedLen := StorerRegisterHeaderSize + tokenLen + StorerRegisterDiskIDSize + AuthProofSize
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

	return StorerRegisterRequest{
		GatewayToken: token,
		DiskID:       diskID,
		AuthVerifier: authVerifier,
	}, nil
}
