package gateway

import (
	"crypto/rand"
	"errors"
	"math/big"
	"time"

	"yumedisk/server/internal/auth"
	"yumedisk/server/internal/proto"
)

const (
	defaultChallengeTTL = 30 * time.Second
	minAuthFailDelay    = 2 * time.Second
	maxAuthFailDelay    = 5 * time.Second
)

type authenticator struct {
	routes       RouteSource
	tokenCodec   *auth.TokenCodec
	challengeTTL time.Duration
	sleep        func(time.Duration)
	randomDelay  func() time.Duration
}

func newAuthenticator(routes RouteSource) (*authenticator, error) {
	tokenCodec, err := auth.NewRandomTokenCodec(32)
	if err != nil {
		return nil, err
	}

	a := &authenticator{
		routes:       routes,
		tokenCodec:   tokenCodec,
		challengeTTL: defaultChallengeTTL,
		sleep:        time.Sleep,
	}
	a.randomDelay = a.defaultRandomDelay
	return a, nil
}

func (a *authenticator) handleAuthStart(state *ConnectionState, header proto.Header, body []byte) ([]byte, error) {
	if header.SessionID != 0 {
		return proto.BuildErrorResponse(header, proto.StatusBadHeader), nil
	}

	diskID, err := proto.ParseAuthStartRequestBody(body)
	if err != nil {
		return proto.BuildErrorResponse(header, proto.StatusBadBody), nil
	}

	challenge, token, err := a.tokenCodec.Issue(state.ID, diskID, a.challengeTTL)
	if err != nil {
		return nil, err
	}

	responseBody, err := proto.BuildAuthStartResponseBody(uint16(a.challengeTTL/time.Second), challenge.Salt[:], token)
	if err != nil {
		return nil, err
	}
	return proto.BuildSuccessResponse(header, responseBody), nil
}

func (a *authenticator) handleAuthFinish(state *ConnectionState, header proto.Header, body []byte) ([]byte, error) {
	if header.SessionID != 0 {
		return proto.BuildErrorResponse(header, proto.StatusBadHeader), nil
	}

	token, proof, err := proto.ParseAuthFinishRequestBody(body)
	if err != nil {
		return proto.BuildErrorResponse(header, proto.StatusBadBody), nil
	}

	challenge, err := a.tokenCodec.Parse(state.ID, token)
	if err != nil {
		if errors.Is(err, auth.ErrChallengeExpired) {
			a.sleep(a.randomDelay())
			return proto.BuildErrorResponse(header, proto.StatusAuthExpired), nil
		}
		a.sleep(a.randomDelay())
		return proto.BuildErrorResponse(header, proto.StatusAuthChallengeInvalid), nil
	}

	entry, ok := a.routes.LookupRoute(challenge.DiskID)
	if !ok {
		a.sleep(a.randomDelay())
		return proto.BuildErrorResponse(header, proto.StatusAuthFailed), nil
	}

	expected := auth.ComputeProof(entry.AuthVerifier, challenge.Salt[:])
	if proof != expected {
		a.sleep(a.randomDelay())
		return proto.BuildErrorResponse(header, proto.StatusAuthFailed), nil
	}

	state.markAuthenticated(challenge.DiskID)
	return proto.BuildSuccessResponse(header, proto.BuildAuthFinishResponseBody()), nil
}

func (a *authenticator) defaultRandomDelay() time.Duration {
	span := maxAuthFailDelay - minAuthFailDelay
	if span <= 0 {
		return minAuthFailDelay
	}

	n, err := rand.Int(rand.Reader, big.NewInt(span.Nanoseconds()+1))
	if err != nil {
		return minAuthFailDelay
	}
	return minAuthFailDelay + time.Duration(n.Int64())
}
