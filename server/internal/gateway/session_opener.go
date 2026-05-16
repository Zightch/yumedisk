package gateway

import (
	"yumedisk/server/internal/proto"
	"yumedisk/server/internal/session"
)

type sessionOpener struct {
	realDiskID string
	sessions   *session.Service
}

func newSessionOpener(realDiskID string, sessions *session.Service) *sessionOpener {
	return &sessionOpener{
		realDiskID: realDiskID,
		sessions:   sessions,
	}
}

func (o *sessionOpener) handleSessionOpen(state *ConnectionState, header proto.Header, body []byte) ([]byte, error) {
	if header.SessionID != 0 {
		return proto.BuildErrorResponse(header, proto.StatusBadHeader), nil
	}

	diskID, err := proto.ParseSessionOpenRequestBody(body)
	if err != nil {
		return proto.BuildErrorResponse(header, proto.StatusBadBody), nil
	}
	if diskID != o.realDiskID || !state.isAuthenticated(diskID) {
		return proto.BuildErrorResponse(header, proto.StatusAuthRequired), nil
	}

	desc := o.sessions.Open(state.ID, diskID)
	bodyOut := proto.BuildSessionOpenResponseBody(desc.DiskSize, desc.MaxIOBytes, o.sessions.TTLSeconds(), desc.ReadOnly)
	respHeader := proto.Header{
		ProtocolVersion: header.ProtocolVersion,
		HeaderLen:       header.HeaderLen,
		OpCode:          header.OpCode,
		Flags:           proto.FlagResponse,
		StatusCode:      proto.StatusOK,
		Reserved:        0,
		RequestID:       header.RequestID,
		SessionID:       desc.ID,
	}
	return proto.BuildResponse(respHeader, proto.StatusOK, bodyOut), nil
}

func (o *sessionOpener) handlePing(header proto.Header, body []byte) ([]byte, error) {
	if header.SessionID == 0 {
		return proto.BuildErrorResponse(header, proto.StatusBadHeader), nil
	}

	nonce, err := proto.ParsePingRequestBody(body)
	if err != nil {
		return proto.BuildErrorResponse(header, proto.StatusBadBody), nil
	}

	_, ok := o.sessions.Ping(header.SessionID)
	if !ok {
		return proto.BuildErrorResponse(header, proto.StatusSessionNotFound), nil
	}
	return proto.BuildSuccessResponse(header, proto.BuildPingResponseBody(nonce)), nil
}

func (o *sessionOpener) handleClose(header proto.Header, body []byte) ([]byte, error) {
	if header.SessionID == 0 {
		return proto.BuildErrorResponse(header, proto.StatusBadHeader), nil
	}
	if len(body) != 0 {
		return proto.BuildErrorResponse(header, proto.StatusBadBody), nil
	}

	o.sessions.Close(header.SessionID)
	return proto.BuildSuccessResponse(header, nil), nil
}
