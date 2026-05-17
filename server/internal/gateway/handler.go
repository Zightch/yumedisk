package gateway

import (
	"errors"
	"fmt"
	"sync"

	"yumedisk/server/internal/proto"
)

type Handler struct {
	authenticator        *authenticator
	sessionOpener        *sessionOpener
	clientDisconnectHook clientDisconnectHandler
}

type ConnectionState struct {
	ID uint64

	mu              sync.RWMutex
	pendingAuthDisk string
	authorizedDisk  string
	openSessionID   uint64
}

type ConnectionHandler struct {
	parent *Handler
	state  *ConnectionState
}

type clientDisconnectHandler interface {
	CloseClientConnection(connectionID uint64)
}

func NewHandler(routes RouteSource, sessions SessionDataPlane) (*Handler, error) {
	if routes == nil {
		return nil, errors.New("gateway handler requires route source")
	}
	if sessions == nil {
		return nil, errors.New("gateway handler requires session data plane")
	}

	authenticator, err := newAuthenticator(routes)
	if err != nil {
		return nil, err
	}
	return &Handler{
		authenticator: authenticator,
		sessionOpener: newSessionOpener(routes, sessions),
	}, nil
}

func (h *Handler) NewConnectionState(id uint64) *ConnectionState {
	return &ConnectionState{
		ID: id,
	}
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
	case proto.OpPing:
		return h.sessionOpener.handlePing(state, header, body)
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
	h.sessionOpener.closeConnection(connectionID)
}

func (h *Handler) CloseRouteConnection(routeConnectionID uint64) {
	sessions := h.sessionOpener.closeRouteConnection(routeConnectionID)
	if h.clientDisconnectHook == nil {
		return
	}

	seen := make(map[uint64]struct{})
	for _, mapped := range sessions {
		if _, ok := seen[mapped.ClientConnection]; ok {
			continue
		}
		seen[mapped.ClientConnection] = struct{}{}
		h.clientDisconnectHook.CloseClientConnection(mapped.ClientConnection)
	}
}

func (h *Handler) SetClientDisconnectHandler(handler clientDisconnectHandler) {
	h.clientDisconnectHook = handler
}

var _ routeDisconnectHandler = (*Handler)(nil)

func (s *ConnectionState) markAuthenticated(diskID string) {
	s.mu.Lock()
	s.pendingAuthDisk = ""
	s.authorizedDisk = diskID
	s.openSessionID = 0
	s.mu.Unlock()
}

func (s *ConnectionState) isAuthenticated(diskID string) bool {
	s.mu.RLock()
	ok := s.authorizedDisk == diskID && s.pendingAuthDisk == ""
	s.mu.RUnlock()
	return ok
}

func (s *ConnectionState) beginAuth(diskID string) error {
	s.mu.Lock()
	defer s.mu.Unlock()
	if s.pendingAuthDisk != "" || s.authorizedDisk != "" || s.openSessionID != 0 {
		return errPhaseViolation
	}
	s.pendingAuthDisk = diskID
	return nil
}

func (s *ConnectionState) finishAuth(diskID string) error {
	s.mu.Lock()
	defer s.mu.Unlock()
	if s.pendingAuthDisk == "" || s.pendingAuthDisk != diskID || s.authorizedDisk != "" || s.openSessionID != 0 {
		return errPhaseViolation
	}
	s.pendingAuthDisk = ""
	s.authorizedDisk = diskID
	return nil
}

func (s *ConnectionState) failAuth() {
	s.mu.Lock()
	s.pendingAuthDisk = ""
	s.mu.Unlock()
}

func (s *ConnectionState) beginSessionOpen(diskID string) error {
	s.mu.Lock()
	defer s.mu.Unlock()
	if s.pendingAuthDisk != "" || s.authorizedDisk == "" || s.authorizedDisk != diskID || s.openSessionID != 0 {
		return errPhaseViolation
	}
	return nil
}

func (s *ConnectionState) finishSessionOpen(sessionID uint64) error {
	s.mu.Lock()
	defer s.mu.Unlock()
	if s.authorizedDisk == "" || s.pendingAuthDisk != "" || s.openSessionID != 0 || sessionID == 0 {
		return errPhaseViolation
	}
	s.openSessionID = sessionID
	return nil
}

func (s *ConnectionState) hasOpenSession(sessionID uint64) bool {
	s.mu.RLock()
	ok := s.openSessionID != 0 && s.openSessionID == sessionID
	s.mu.RUnlock()
	return ok
}

func (s *ConnectionState) pendingAuth() (string, bool) {
	s.mu.RLock()
	diskID := s.pendingAuthDisk
	s.mu.RUnlock()
	return diskID, diskID != ""
}

func (s *ConnectionState) requireOpenSession(sessionID uint64) error {
	s.mu.RLock()
	defer s.mu.RUnlock()
	if s.pendingAuthDisk != "" || s.authorizedDisk == "" || s.openSessionID == 0 || s.openSessionID != sessionID {
		return errPhaseViolation
	}
	return nil
}

func (s *ConnectionState) clearSession(sessionID uint64) bool {
	s.mu.Lock()
	defer s.mu.Unlock()
	if s.openSessionID == 0 || s.openSessionID != sessionID {
		return false
	}
	s.openSessionID = 0
	return true
}

var (
	errPhaseViolation = errors.New("connection phase violation")
)
