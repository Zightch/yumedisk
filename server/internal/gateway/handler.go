package gateway

import (
	"errors"
	"fmt"
	"sync"

	"yumedisk/server/internal/proto"
)

type Handler struct {
	authenticator *authenticator
	sessionOpener *sessionOpener
	grants        *authGrantRegistry

	noticeMu             sync.RWMutex
	sessionCloseNotifier sessionCloseNotifier
}

type ConnectionState struct {
	ID uint64

	mu                sync.RWMutex
	authInFlight      bool
	openInFlight      bool
	heartbeatWatchdog *clientHeartbeatWatchdog
}

type ConnectionHandler struct {
	parent *Handler
	state  *ConnectionState
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
		sessionOpener: newSessionOpener(routes, sessions, grants),
		grants:        grants,
	}, nil
}

func (h *Handler) NewConnectionState(id uint64) *ConnectionState {
	return &ConnectionState{ID: id}
}

func (h *Handler) Bind(state *ConnectionState) *ConnectionHandler {
	return &ConnectionHandler{
		parent: h,
		state:  state,
	}
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
		return h.sessionOpener.handleSessionOpen(state, header, body)
	case proto.OpSessionDescribe:
		return h.sessionOpener.handleDescribe(state, header, body)
	case proto.OpConnHeartbeat:
		return h.sessionOpener.handleConnHeartbeat(state, header, body)
	case proto.OpClose:
		return h.sessionOpener.handleClose(state, header, body)
	case proto.OpReadAt:
		return h.sessionOpener.handleRead(state, header, body)
	case proto.OpWriteAt:
		return h.sessionOpener.handleWrite(state, header, body)
	default:
		return proto.BuildErrorResponse(header, proto.StatusUnsupportedOp), nil
	}
}

func (h *ConnectionHandler) HandlePayload(payload []byte) ([]byte, error) {
	return h.parent.HandlePayload(h.state, payload)
}

func (h *Handler) CloseConnection(connectionID uint64) {
	h.grants.CloseConnection(connectionID)
	h.sessionOpener.closeConnection(connectionID)
}

func (h *Handler) CloseRouteConnection(routeConnectionID uint64, diskIDs []string) {
	for _, diskID := range diskIDs {
		h.grants.CloseDisk(diskID)
	}
	h.emitSessionClosed(
		h.sessionOpener.closeRouteConnection(routeConnectionID),
		proto.SessionCloseReasonRouteLost,
	)
}

func (h *Handler) SetSessionCloseNotifier(notifier sessionCloseNotifier) {
	h.noticeMu.Lock()
	h.sessionCloseNotifier = notifier
	h.noticeMu.Unlock()
}

func (h *Handler) closeRouteConnectionSessions(routeConnectionID uint64, diskIDs []string) []gatewaySessionRecord {
	for _, diskID := range diskIDs {
		h.grants.CloseDisk(diskID)
	}
	return h.sessionOpener.closeRouteConnection(routeConnectionID)
}

func (h *Handler) emitSessionClosed(records []gatewaySessionRecord, reason uint16) {
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
		notifier.NotifySessionClosed(record, reason)
	}
}

func (s *ConnectionState) beginAuth() error {
	s.mu.Lock()
	defer s.mu.Unlock()
	if s.authInFlight || s.openInFlight {
		return errPhaseViolation
	}
	s.authInFlight = true
	return nil
}

func (s *ConnectionState) finishAuth() error {
	s.mu.Lock()
	defer s.mu.Unlock()
	if !s.authInFlight || s.openInFlight {
		return errPhaseViolation
	}
	s.authInFlight = false
	return nil
}

func (s *ConnectionState) failAuth() {
	s.mu.Lock()
	s.authInFlight = false
	s.mu.Unlock()
}

func (s *ConnectionState) beginSessionOpen() error {
	s.mu.Lock()
	defer s.mu.Unlock()
	if s.authInFlight || s.openInFlight {
		return errPhaseViolation
	}
	s.openInFlight = true
	return nil
}

func (s *ConnectionState) finishSessionOpen() error {
	s.mu.Lock()
	defer s.mu.Unlock()
	if !s.openInFlight || s.authInFlight {
		return errPhaseViolation
	}
	s.openInFlight = false
	return nil
}

func (s *ConnectionState) failSessionOpen() {
	s.mu.Lock()
	s.openInFlight = false
	s.mu.Unlock()
}

func (s *ConnectionState) pendingAuth() bool {
	s.mu.RLock()
	pending := s.authInFlight
	s.mu.RUnlock()
	return pending
}

func (s *ConnectionState) setHeartbeatWatchdog(watchdog *clientHeartbeatWatchdog) {
	s.mu.Lock()
	s.heartbeatWatchdog = watchdog
	s.mu.Unlock()
}

func (s *ConnectionState) markHeartbeat() {
	s.mu.RLock()
	watchdog := s.heartbeatWatchdog
	s.mu.RUnlock()
	if watchdog != nil {
		watchdog.Mark()
	}
}

var (
	errPhaseViolation = errors.New("connection phase violation")
)
