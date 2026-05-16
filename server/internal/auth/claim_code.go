package auth

import (
	"crypto/hmac"
	"crypto/rand"
	"crypto/sha512"
	"errors"
	"fmt"
)

const (
	DiskIDLength         = 16
	MinClaimSecretLength = 64
	MinClaimCodeLength   = DiskIDLength + MinClaimSecretLength
)

const claimCodeAlphabet = "0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ"

type Material struct {
	ClaimCode    string
	DiskID       string
	AuthVerifier [64]byte
}

func GenerateClaimCode(secretLen int) (string, error) {
	if secretLen < MinClaimSecretLength {
		return "", fmt.Errorf("secret length must be >= %d", MinClaimSecretLength)
	}

	raw := make([]byte, DiskIDLength+secretLen)
	if _, err := rand.Read(raw); err != nil {
		return "", fmt.Errorf("generate random claim code bytes: %w", err)
	}

	out := make([]byte, len(raw))
	for i, b := range raw {
		out[i] = claimCodeAlphabet[int(b)%len(claimCodeAlphabet)]
	}
	return string(out), nil
}

func ParseClaimCode(claimCode string) (Material, error) {
	if len(claimCode) < MinClaimCodeLength {
		return Material{}, fmt.Errorf("claim code length must be >= %d", MinClaimCodeLength)
	}
	if !isAlphaNumericASCII(claimCode) {
		return Material{}, errors.New("claim code must use 0-9a-zA-Z only")
	}

	return Material{
		ClaimCode:    claimCode,
		DiskID:       claimCode[:DiskIDLength],
		AuthVerifier: ComputeAuthVerifier(claimCode),
	}, nil
}

func ComputeAuthVerifier(claimCode string) [64]byte {
	return sha512.Sum512([]byte(claimCode))
}

func ComputeProof(authVerifier [64]byte, salt []byte) [64]byte {
	mac := hmac.New(sha512.New, authVerifier[:])
	_, _ = mac.Write(salt)

	var proof [64]byte
	copy(proof[:], mac.Sum(nil))
	return proof
}

func isAlphaNumericASCII(s string) bool {
	for i := range len(s) {
		c := s[i]
		isDigit := c >= '0' && c <= '9'
		isLower := c >= 'a' && c <= 'z'
		isUpper := c >= 'A' && c <= 'Z'
		if !(isDigit || isLower || isUpper) {
			return false
		}
	}
	return true
}
