package client

import (
	"errors"
	"fmt"
	"sync"

	clientsession "yumedisk/server/internal/gateway/client/session"
	"yumedisk/server/internal/proto"
)

type Handler struct {
	authenticator *authenticator
	sessionOpener *clientsession.Opener
	grants        *authGrantRegistry

	noticeMu             sync.RWMutex
	sessionCloseNotifier clientsession.CloseNotifier
}

func NewHandler(routes RouteSource, sessions SessionDataPlane) (*Handler, error) {
	if routes == nil {
		return nil, errors.New("gateway handler requires route source")
	}
	if sessions == nil {
		return nil, errors.New("gateway handler requires session data plane")
	}

	grants := newAuthGrantRegistry()
	authenticator, err := newAuthenticator(routes, grants)
	if err != nil {
		return nil, err
	}
	return &Handler{
		authenticator: authenticator,
		sessionOpener: clientsession.NewOpener(routes, sessions, grants),
		grants:        grants,
	}, nil
}

func (h *Handler) NewConnectionState(id uint64) *ConnectionState {
	return &ConnectionState{ID: id}
}

func (h *Handler) Bind(state *ConnectionState) *ConnectionHandler {
	return newConnectionHandler(h, state)
}

func (h *Handler) HandlePayload(state *ConnectionState, payload []byte) ([]byte, error) {
	header, err := proto.ParseHeader(payload)
	if err != nil {
		return nil, fmt.Errorf("connection %d parse header: %w", state.ID, err)
	}
	if err := proto.ValidateRequestHeader(header); err != nil {
		return proto.BuildErrorResponse(header, proto.StatusBadHeader), nil
	}

	body := payload[proto.HeaderSize:]
	switch header.OpCode {
	case proto.OpAuthStart:
		return h.authenticator.handleAuthStart(state, header, body)
	case proto.OpAuthFinish:
		return h.authenticator.handleAuthFinish(state, header, body)
	case proto.OpSessionOpen:
		return h.sessionOpener.HandleSessionOpen(state, header, body)
	case proto.OpSessionDescribe:
		return h.sessionOpener.HandleDescribe(state, header, body)
	case proto.OpConnHeartbeat:
		return h.sessionOpener.HandleConnHeartbeat(state, header, body)
	case proto.OpClose:
		return h.sessionOpener.HandleClose(state, header, body)
	case proto.OpReadAt:
		return h.sessionOpener.HandleRead(state, header, body)
	case proto.OpWriteAt:
		return h.sessionOpener.HandleWrite(state, header, body)
	default:
		return proto.BuildErrorResponse(header, proto.StatusUnsupportedOp), nil
	}
}

func (h *Handler) CloseConnection(connectionID uint64) {
	h.grants.CloseConnection(connectionID)
	h.sessionOpener.CloseConnection(connectionID)
}

func (h *Handler) CloseRouteConnection(routeConnectionID uint64, diskIDs []string) {
	for _, diskID := range diskIDs {
		h.grants.CloseDisk(diskID)
	}
	h.emitSessionClosed(
		h.sessionOpener.CloseRouteConnection(routeConnectionID),
		proto.SessionCloseReasonRouteLost,
	)
}

func (h *Handler) SetSessionCloseNotifier(notifier clientsession.CloseNotifier) {
	h.noticeMu.Lock()
	h.sessionCloseNotifier = notifier
	h.noticeMu.Unlock()
}

func (h *Handler) closeRouteConnectionSessions(routeConnectionID uint64, diskIDs []string) []clientsession.Record {
	for _, diskID := range diskIDs {
		h.grants.CloseDisk(diskID)
	}
	return h.sessionOpener.CloseRouteConnection(routeConnectionID)
}

func (h *Handler) emitSessionClosed(records []clientsession.Record, reason uint16) {
	if len(records) == 0 {
		return
	}

	h.noticeMu.RLock()
	notifier := h.sessionCloseNotifier
	h.noticeMu.RUnlock()
	if notifier == nil {
		return
	}
	for _, record := range records {
		notifier.NotifySessionClosed(record.ID, record.ClientConnectionID, reason)
	}
}
