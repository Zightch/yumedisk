package client

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
	grants       *authGrantRegistry
	tokenCodec   *auth.TokenCodec
	challengeTTL time.Duration
	sleep        func(time.Duration)
	randomDelay  func() time.Duration
}

func newAuthenticator(routes RouteSource, grants *authGrantRegistry) (*authenticator, error) {
	tokenCodec, err := auth.NewRandomTokenCodec(32)
	if err != nil {
		return nil, err
	}

	a := &authenticator{
		routes:       routes,
		grants:       grants,
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
	if err := state.beginAuth(); err != nil {
		return proto.BuildErrorResponse(header, proto.StatusInvalidRequest), nil
	}

	challenge, token, err := a.tokenCodec.Issue(state.ID, diskID, a.challengeTTL)
	if err != nil {
		state.failAuth()
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

	if !state.pendingAuth() {
		return proto.BuildErrorResponse(header, proto.StatusInvalidRequest), nil
	}

	token, proof, err := proto.ParseAuthFinishRequestBody(body)
	if err != nil {
		state.failAuth()
		return proto.BuildErrorResponse(header, proto.StatusBadBody), nil
	}

	challenge, err := a.tokenCodec.Parse(state.ID, token)
	if err != nil {
		state.failAuth()
		if errors.Is(err, auth.ErrChallengeExpired) {
			a.sleep(a.randomDelay())
			return proto.BuildErrorResponse(header, proto.StatusAuthExpired), nil
		}
		a.sleep(a.randomDelay())
		return proto.BuildErrorResponse(header, proto.StatusAuthChallengeInvalid), nil
	}

	entry, ok := a.routes.LookupRoute(challenge.DiskID)
	if !ok {
		state.failAuth()
		a.sleep(a.randomDelay())
		return proto.BuildErrorResponse(header, proto.StatusAuthFailed), nil
	}

	expected := auth.ComputeProof(entry.AuthVerifier, challenge.Salt[:])
	if proof != expected {
		state.failAuth()
		a.sleep(a.randomDelay())
		return proto.BuildErrorResponse(header, proto.StatusAuthFailed), nil
	}

	authID := a.grants.Issue(state.ID, challenge.DiskID, time.Now().Add(a.challengeTTL))
	if err := state.finishAuth(); err != nil {
		state.failAuth()
		return proto.BuildErrorResponse(header, proto.StatusInvalidRequest), nil
	}
	return proto.BuildSuccessResponse(header, proto.BuildAuthFinishResponseBody(authID)), nil
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
