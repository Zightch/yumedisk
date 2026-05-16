package proto

import (
	"encoding/binary"
	"errors"
)

const (
	AuthDiskIDSize    = 16
	AuthSaltSize      = 16
	AuthTokenMinSize  = 1
	AuthProofSize     = 64
	AuthAlgoVersionV1 = 1
)

var ErrAuthBody = errors.New("auth body invalid")

func ParseAuthStartRequestBody(body []byte) (string, error) {
	if len(body) != AuthDiskIDSize {
		return "", ErrAuthBody
	}
	if !isAlphaNumericASCII(body) {
		return "", ErrAuthBody
	}
	return string(body), nil
}

func BuildAuthStartResponseBody(ttlSeconds uint16, salt []byte, challengeToken []byte) ([]byte, error) {
	if len(salt) != AuthSaltSize {
		return nil, ErrAuthBody
	}
	if len(challengeToken) < AuthTokenMinSize || len(challengeToken) > 65535 {
		return nil, ErrAuthBody
	}

	body := make([]byte, 1+2+AuthSaltSize+2+len(challengeToken))
	body[0] = AuthAlgoVersionV1
	binary.BigEndian.PutUint16(body[1:3], ttlSeconds)
	copy(body[3:19], salt)
	binary.BigEndian.PutUint16(body[19:21], uint16(len(challengeToken)))
	copy(body[21:], challengeToken)
	return body, nil
}

type AuthStartResponseBody struct {
	AlgoVersion    uint8
	TTLSeconds     uint16
	Salt           [AuthSaltSize]byte
	ChallengeToken []byte
}

func ParseAuthStartResponseBody(body []byte) (AuthStartResponseBody, error) {
	var out AuthStartResponseBody
	if len(body) < 21 {
		return out, ErrAuthBody
	}

	out.AlgoVersion = body[0]
	out.TTLSeconds = binary.BigEndian.Uint16(body[1:3])
	copy(out.Salt[:], body[3:19])

	tokenLen := int(binary.BigEndian.Uint16(body[19:21]))
	if tokenLen < AuthTokenMinSize {
		return out, ErrAuthBody
	}
	if len(body) != 21+tokenLen {
		return out, ErrAuthBody
	}

	out.ChallengeToken = make([]byte, tokenLen)
	copy(out.ChallengeToken, body[21:])
	return out, nil
}

func ParseAuthFinishRequestBody(body []byte) ([]byte, [AuthProofSize]byte, error) {
	var proof [AuthProofSize]byte
	if len(body) < 2+AuthProofSize {
		return nil, proof, ErrAuthBody
	}

	tokenLen := int(binary.BigEndian.Uint16(body[:2]))
	if tokenLen < AuthTokenMinSize {
		return nil, proof, ErrAuthBody
	}
	if len(body) != 2+tokenLen+AuthProofSize {
		return nil, proof, ErrAuthBody
	}

	token := make([]byte, tokenLen)
	copy(token, body[2:2+tokenLen])
	copy(proof[:], body[2+tokenLen:])
	return token, proof, nil
}

func BuildAuthFinishRequestBody(challengeToken []byte, proof [AuthProofSize]byte) []byte {
	body := make([]byte, 2+len(challengeToken)+AuthProofSize)
	binary.BigEndian.PutUint16(body[:2], uint16(len(challengeToken)))
	copy(body[2:2+len(challengeToken)], challengeToken)
	copy(body[2+len(challengeToken):], proof[:])
	return body
}

func BuildAuthFinishResponseBody() []byte {
	return nil
}

func isAlphaNumericASCII(b []byte) bool {
	for _, c := range b {
		isDigit := c >= '0' && c <= '9'
		isLower := c >= 'a' && c <= 'z'
		isUpper := c >= 'A' && c <= 'Z'
		if !(isDigit || isLower || isUpper) {
			return false
		}
	}
	return true
}
