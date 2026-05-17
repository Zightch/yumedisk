package storer

import (
	"fmt"

	"yumedisk/server/internal/proto"
	"yumedisk/server/internal/session"
)

type dataPlaneHandler struct {
	connectionID uint64
	diskID       string
	sessions     *session.Service
}

func newDataPlaneHandler(connectionID uint64, diskID string, sessions *session.Service) *dataPlaneHandler {
	return &dataPlaneHandler{
		connectionID: connectionID,
		diskID:       diskID,
		sessions:     sessions,
	}
}

func (h *dataPlaneHandler) HandlePayload(payload []byte) ([]byte, error) {
	header, err := proto.ParseHeader(payload)
	if err != nil {
		return nil, fmt.Errorf("storer connection %d parse header: %w", h.connectionID, err)
	}
	if err := proto.ValidateRequestHeader(header); err != nil {
		return proto.BuildErrorResponse(header, proto.StatusBadHeader), nil
	}

	body := payload[proto.HeaderSize:]
	switch header.OpCode {
	case proto.OpSessionOpen:
		return h.handleSessionOpen(header, body)
	case proto.OpLinkHeartbeat:
		return h.handleLinkHeartbeat(header, body)
	case proto.OpPing:
		return h.handlePing(header, body)
	case proto.OpClose:
		return h.handleClose(header, body)
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

	diskID, err := proto.ParseSessionOpenRequestBody(body)
	if err != nil {
		return proto.BuildErrorResponse(header, proto.StatusBadBody), nil
	}
	if diskID != h.diskID {
		return proto.BuildErrorResponse(header, proto.StatusSessionUnavailable), nil
	}

	desc, err := h.sessions.Open(h.connectionID, diskID)
	if err != nil {
		return h.mapSessionError(header, err), nil
	}
	bodyOut := proto.BuildSessionOpenResponseBody(desc.DiskSize, desc.MaxIOBytes, h.sessions.TTLSeconds(), desc.ReadOnly)
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

func (h *dataPlaneHandler) handleLinkHeartbeat(header proto.Header, body []byte) ([]byte, error) {
	if header.SessionID != 0 {
		return proto.BuildErrorResponse(header, proto.StatusBadHeader), nil
	}
	nonce, err := proto.ParsePingRequestBody(body)
	if err != nil {
		return proto.BuildErrorResponse(header, proto.StatusBadBody), nil
	}
	return proto.BuildSuccessResponse(header, proto.BuildPingResponseBody(nonce)), nil
}

func (h *dataPlaneHandler) handlePing(header proto.Header, body []byte) ([]byte, error) {
	if header.SessionID == 0 {
		return proto.BuildErrorResponse(header, proto.StatusBadHeader), nil
	}

	nonce, err := proto.ParsePingRequestBody(body)
	if err != nil {
		return proto.BuildErrorResponse(header, proto.StatusBadBody), nil
	}

	_, ok := h.sessions.Ping(header.SessionID)
	if !ok {
		return proto.BuildErrorResponse(header, proto.StatusSessionUnavailable), nil
	}
	return proto.BuildSuccessResponse(header, proto.BuildPingResponseBody(nonce)), nil
}

func (h *dataPlaneHandler) handleClose(header proto.Header, body []byte) ([]byte, error) {
	if header.SessionID == 0 {
		return proto.BuildErrorResponse(header, proto.StatusBadHeader), nil
	}
	if len(body) != 0 {
		return proto.BuildErrorResponse(header, proto.StatusBadBody), nil
	}

	h.sessions.Close(header.SessionID)
	return proto.BuildSuccessResponse(header, nil), nil
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
	case session.ErrSessionBusy:
		return proto.BuildErrorResponse(header, proto.StatusSessionBusy)
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
