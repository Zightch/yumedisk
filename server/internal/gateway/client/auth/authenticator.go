package clientauth

import (
	"crypto/rand"
	"errors"
	"math/big"
	"time"

	serverauth "yumedisk/server/internal/auth"
	"yumedisk/server/internal/proto"
)

const (
	DefaultChallengeTTL = 30 * time.Second
	MinAuthFailDelay    = 2 * time.Second
	MaxAuthFailDelay    = 5 * time.Second
)

type Authenticator struct {
	routes       RouteSource
	grants       *Registry
	tokenCodec   *serverauth.TokenCodec
	challengeTTL time.Duration
	sleep        func(time.Duration)
	randomDelay  func() time.Duration
}

func NewAuthenticator(routes RouteSource, grants *Registry) (*Authenticator, error) {
	tokenCodec, err := serverauth.NewRandomTokenCodec(32)
	if err != nil {
		return nil, err
	}

	a := &Authenticator{
		routes:       routes,
		grants:       grants,
		tokenCodec:   tokenCodec,
		challengeTTL: DefaultChallengeTTL,
		sleep:        time.Sleep,
	}
	a.randomDelay = a.defaultRandomDelay
	return a, nil
}

func (a *Authenticator) HandleAuthStart(state ConnectionState, header proto.Header, body []byte) ([]byte, error) {
	if header.SessionID != 0 {
		return proto.BuildErrorResponse(header, proto.StatusBadHeader), nil
	}

	diskID, err := proto.ParseAuthStartRequestBody(body)
	if err != nil {
		return proto.BuildErrorResponse(header, proto.StatusBadBody), nil
	}
	if err := state.BeginAuth(); err != nil {
		return proto.BuildErrorResponse(header, proto.StatusInvalidRequest), nil
	}

	challenge, token, err := a.tokenCodec.Issue(state.ConnectionID(), diskID, a.challengeTTL)
	if err != nil {
		state.FailAuth()
		return nil, err
	}

	responseBody, err := proto.BuildAuthStartResponseBody(uint16(a.challengeTTL/time.Second), challenge.Salt[:], token)
	if err != nil {
		return nil, err
	}
	return proto.BuildSuccessResponse(header, responseBody), nil
}

func (a *Authenticator) HandleAuthFinish(state ConnectionState, header proto.Header, body []byte) ([]byte, error) {
	if header.SessionID != 0 {
		return proto.BuildErrorResponse(header, proto.StatusBadHeader), nil
	}

	if !state.PendingAuth() {
		return proto.BuildErrorResponse(header, proto.StatusInvalidRequest), nil
	}

	token, proof, err := proto.ParseAuthFinishRequestBody(body)
	if err != nil {
		state.FailAuth()
		return proto.BuildErrorResponse(header, proto.StatusBadBody), nil
	}

	challenge, err := a.tokenCodec.Parse(state.ConnectionID(), token)
	if err != nil {
		state.FailAuth()
		if errors.Is(err, serverauth.ErrChallengeExpired) {
			a.sleep(a.randomDelay())
			return proto.BuildErrorResponse(header, proto.StatusAuthExpired), nil
		}
		a.sleep(a.randomDelay())
		return proto.BuildErrorResponse(header, proto.StatusAuthChallengeInvalid), nil
	}

	entry, ok := a.routes.LookupRoute(challenge.DiskID)
	if !ok {
		state.FailAuth()
		a.sleep(a.randomDelay())
		return proto.BuildErrorResponse(header, proto.StatusAuthFailed), nil
	}

	expected := serverauth.ComputeProof(entry.AuthVerifier, challenge.Salt[:])
	if proof != expected {
		state.FailAuth()
		a.sleep(a.randomDelay())
		return proto.BuildErrorResponse(header, proto.StatusAuthFailed), nil
	}

	authID := a.grants.Issue(state.ConnectionID(), challenge.DiskID, time.Now().Add(a.challengeTTL))
	if err := state.FinishAuth(); err != nil {
		state.FailAuth()
		return proto.BuildErrorResponse(header, proto.StatusInvalidRequest), nil
	}
	return proto.BuildSuccessResponse(header, proto.BuildAuthFinishResponseBody(authID)), nil
}

func (a *Authenticator) RouteSource() RouteSource {
	return a.routes
}

func (a *Authenticator) SetFailureDelayHooks(sleep func(time.Duration), randomDelay func() time.Duration) {
	if sleep != nil {
		a.sleep = sleep
	}
	if randomDelay != nil {
		a.randomDelay = randomDelay
	}
}

func (a *Authenticator) defaultRandomDelay() time.Duration {
	span := MaxAuthFailDelay - MinAuthFailDelay
	if span <= 0 {
		return MinAuthFailDelay
	}

	n, err := rand.Int(rand.Reader, big.NewInt(span.Nanoseconds()+1))
	if err != nil {
		return MinAuthFailDelay
	}
	return MinAuthFailDelay + time.Duration(n.Int64())
}
