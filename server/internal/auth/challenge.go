package auth

import (
	"crypto/hmac"
	"crypto/rand"
	"crypto/sha512"
	"encoding/binary"
	"errors"
	"fmt"
	"time"
)

const (
	ChallengeSaltSize  = 16
	ChallengeTokenSize = 1 + 8 + 8 + DiskIDLength + ChallengeSaltSize + 64
	challengeVersionV1 = 1
)

var (
	ErrChallengeTokenInvalid = errors.New("challenge token invalid")
	ErrChallengeExpired      = errors.New("challenge expired")
)

type Challenge struct {
	DiskID    string
	Salt      [ChallengeSaltSize]byte
	ExpiresAt time.Time
}

type TokenCodec struct {
	secret []byte
	now    func() time.Time
}

func NewTokenCodec(secret []byte) *TokenCodec {
	copied := make([]byte, len(secret))
	copy(copied, secret)
	return &TokenCodec{
		secret: copied,
		now:    time.Now,
	}
}

func NewRandomTokenCodec(secretSize int) (*TokenCodec, error) {
	if secretSize <= 0 {
		return nil, fmt.Errorf("secret size must be > 0")
	}

	secret := make([]byte, secretSize)
	if _, err := rand.Read(secret); err != nil {
		return nil, fmt.Errorf("generate token codec secret: %w", err)
	}
	return NewTokenCodec(secret), nil
}

func (c *TokenCodec) Issue(connectionID uint64, diskID string, ttl time.Duration) (Challenge, []byte, error) {
	if len(diskID) != DiskIDLength {
		return Challenge{}, nil, ErrChallengeTokenInvalid
	}
	if ttl <= 0 {
		return Challenge{}, nil, ErrChallengeTokenInvalid
	}

	var challenge Challenge
	challenge.DiskID = diskID
	if _, err := rand.Read(challenge.Salt[:]); err != nil {
		return Challenge{}, nil, fmt.Errorf("generate challenge salt: %w", err)
	}
	challenge.ExpiresAt = c.now().Add(ttl)

	token := make([]byte, ChallengeTokenSize)
	token[0] = challengeVersionV1
	binary.BigEndian.PutUint64(token[1:9], connectionID)
	binary.BigEndian.PutUint64(token[9:17], uint64(challenge.ExpiresAt.Unix()))
	copy(token[17:33], []byte(diskID))
	copy(token[33:49], challenge.Salt[:])

	mac := hmac.New(sha512.New, c.secret)
	_, _ = mac.Write(token[:49])
	copy(token[49:], mac.Sum(nil))

	return challenge, token, nil
}

func (c *TokenCodec) Parse(connectionID uint64, token []byte) (Challenge, error) {
	if len(token) != ChallengeTokenSize {
		return Challenge{}, ErrChallengeTokenInvalid
	}
	if token[0] != challengeVersionV1 {
		return Challenge{}, ErrChallengeTokenInvalid
	}

	mac := hmac.New(sha512.New, c.secret)
	_, _ = mac.Write(token[:49])
	expected := mac.Sum(nil)
	if !hmac.Equal(expected, token[49:]) {
		return Challenge{}, ErrChallengeTokenInvalid
	}

	tokenConnectionID := binary.BigEndian.Uint64(token[1:9])
	if tokenConnectionID != connectionID {
		return Challenge{}, ErrChallengeTokenInvalid
	}

	expiresAt := time.Unix(int64(binary.BigEndian.Uint64(token[9:17])), 0)
	if !expiresAt.After(c.now()) {
		return Challenge{}, ErrChallengeExpired
	}

	var challenge Challenge
	challenge.DiskID = string(token[17:33])
	copy(challenge.Salt[:], token[33:49])
	challenge.ExpiresAt = expiresAt
	return challenge, nil
}
