package gateway

import (
	"fmt"

	"yumedisk/server/internal/proto"
	"yumedisk/server/internal/session"
)

type dataPlaneHandler struct {
	connectionID uint64
	sessions     *session.Service
	watchdog     *linkHeartbeatWatchdog
}

func newDataPlaneHandler(connectionID uint64, sessions *session.Service, watchdog *linkHeartbeatWatchdog) *dataPlaneHandler {
	return &dataPlaneHandler{
		connectionID: connectionID,
		sessions:     sessions,
		watchdog:     watchdog,
	}
}

func (h *dataPlaneHandler) HandlePayload(payload []byte) ([]byte, error) {
	header, err := proto.ParseHeader(payload)
	if err != nil {
		return nil, fmt.Errorf("storer connection %d parse header: %w", h.connectionID, err)
	}
	body := payload[proto.HeaderSize:]

	if header.Flags == proto.FlagNotice {
		if err := proto.ValidateNoticeHeader(header); err != nil {
			return nil, fmt.Errorf("storer connection %d validate notice: %w", h.connectionID, err)
		}

		switch header.OpCode {
		case proto.OpSessionCloseNotice:
			return h.handleSessionCloseNotice(header, body)
		default:
			return nil, fmt.Errorf("storer connection %d unsupported notice op: %d", h.connectionID, header.OpCode)
		}
	}

	if err := proto.ValidateRequestHeader(header); err != nil {
		return proto.BuildErrorResponse(header, proto.StatusBadHeader), nil
	}

	switch header.OpCode {
	case proto.OpSessionOpen:
		return h.handleSessionOpen(header, body)
	case proto.OpSessionDescribe:
		return h.handleSessionDescribe(header, body)
	case proto.OpLinkHeartbeat:
		return h.handleLinkHeartbeat(header, body)
	case proto.OpReadAt:
		return h.handleRead(header, body)
	case proto.OpWriteAt:
		return h.handleWrite(header, body)
	default:
		return proto.BuildErrorResponse(header, proto.StatusUnsupportedOp), nil
	}
}

func (h *dataPlaneHandler) handleSessionOpen(header proto.Header, body []byte) ([]byte, error) {
	if header.SessionID != 0 {
		return proto.BuildErrorResponse(header, proto.StatusBadHeader), nil
	}
	if len(body) != 0 {
		return proto.BuildErrorResponse(header, proto.StatusBadBody), nil
	}

	desc, err := h.sessions.Open(h.connectionID)
	if err != nil {
		return h.mapSessionError(header, err), nil
	}
	return proto.BuildResponseWithSessionID(header, proto.StatusOK, desc.ID, nil), nil
}

func (h *dataPlaneHandler) handleSessionDescribe(header proto.Header, body []byte) ([]byte, error) {
	if header.SessionID == 0 {
		return proto.BuildErrorResponse(header, proto.StatusBadHeader), nil
	}
	if len(body) != 0 {
		return proto.BuildErrorResponse(header, proto.StatusBadBody), nil
	}

	metadata, err := h.sessions.Describe(header.SessionID)
	if err != nil {
		return h.mapSessionError(header, err), nil
	}
	return proto.BuildSuccessResponse(
		header,
		proto.BuildSessionDescribeResponseBody(
			metadata.DiskSizeBytes,
			metadata.MaxIOBytes,
			metadata.ReadOnly,
			metadata.BackendID,
		),
	), nil
}

func (h *dataPlaneHandler) handleLinkHeartbeat(header proto.Header, body []byte) ([]byte, error) {
	if header.SessionID != 0 {
		return proto.BuildErrorResponse(header, proto.StatusBadHeader), nil
	}
	nonce, err := proto.ParseLinkHeartbeatBody(body)
	if err != nil {
		return proto.BuildErrorResponse(header, proto.StatusBadBody), nil
	}
	if h.watchdog != nil {
		h.watchdog.Mark()
	}
	return proto.BuildSuccessResponse(header, proto.BuildLinkHeartbeatBody(nonce)), nil
}

func (h *dataPlaneHandler) handleSessionCloseNotice(header proto.Header, body []byte) ([]byte, error) {
	if header.SessionID == 0 {
		return nil, fmt.Errorf("session close notice requires session id")
	}
	if _, err := proto.ParseSessionCloseNoticeBody(body); err != nil {
		return nil, fmt.Errorf("session close notice body: %w", err)
	}

	h.sessions.Close(header.SessionID)
	return nil, nil
}

func (h *dataPlaneHandler) handleRead(header proto.Header, body []byte) ([]byte, error) {
	if header.SessionID == 0 {
		return proto.BuildErrorResponse(header, proto.StatusBadHeader), nil
	}

	offset, length, err := proto.ParseReadBody(body)
	if err != nil {
		return proto.BuildErrorResponse(header, proto.StatusBadBody), nil
	}

	data, err := h.sessions.Read(header.SessionID, offset, length)
	if err != nil {
		return h.mapSessionError(header, err), nil
	}
	return proto.BuildSuccessResponse(header, data), nil
}

func (h *dataPlaneHandler) handleWrite(header proto.Header, body []byte) ([]byte, error) {
	if header.SessionID == 0 {
		return proto.BuildErrorResponse(header, proto.StatusBadHeader), nil
	}

	offset, _, data, err := proto.ParseReadWriteBody(body)
	if err != nil {
		return proto.BuildErrorResponse(header, proto.StatusBadBody), nil
	}

	if err := h.sessions.Write(header.SessionID, offset, data); err != nil {
		return h.mapSessionError(header, err), nil
	}
	return proto.BuildSuccessResponse(header, nil), nil
}

func (h *dataPlaneHandler) mapSessionError(header proto.Header, err error) []byte {
	switch err {
	case session.ErrSessionUnavailable:
		return proto.BuildErrorResponse(header, proto.StatusSessionUnavailable)
	case session.ErrSessionOpenRejected:
		return proto.BuildErrorResponse(header, proto.StatusSessionOpenRejected)
	case session.ErrReadOnly:
		return proto.BuildErrorResponse(header, proto.StatusIOReadOnly)
	case session.ErrIOLimit:
		return proto.BuildErrorResponse(header, proto.StatusIOLarge)
	case session.ErrOutOfRange:
		return proto.BuildErrorResponse(header, proto.StatusIOOutOfRange)
	case session.ErrIOFailed:
		return proto.BuildErrorResponse(header, proto.StatusIOFailed)
	default:
		return proto.BuildErrorResponse(header, proto.StatusIOFailed)
	}
}
