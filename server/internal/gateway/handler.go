package gateway

import (
	"fmt"
	"sync"

	"yumedisk/server/internal/proto"
	"yumedisk/server/internal/session"
)

type Handler struct {
	authenticator *authenticator
	sessionOpener *sessionOpener
}

type ConnectionState struct {
	ID uint64

	mu                sync.RWMutex
	authenticatedDisk map[string]struct{}
}

type ConnectionHandler struct {
	parent *Handler
	state  *ConnectionState
}

func NewHandler(realDiskID string, authVerifier [64]byte, sessions *session.Service) (*Handler, error) {
	authenticator, err := newAuthenticator(realDiskID, authVerifier)
	if err != nil {
		return nil, err
	}
	return &Handler{
		authenticator: authenticator,
		sessionOpener: newSessionOpener(realDiskID, sessions),
	}, nil
}

func (h *Handler) NewConnectionState(id uint64) *ConnectionState {
	return &ConnectionState{
		ID:                id,
		authenticatedDisk: make(map[string]struct{}),
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
		return h.sessionOpener.handlePing(header, body)
	case proto.OpClose:
		return h.sessionOpener.handleClose(header, body)
	default:
		return proto.BuildErrorResponse(header, proto.StatusUnsupportedOp), nil
	}
}

func (h *ConnectionHandler) HandlePayload(payload []byte) ([]byte, error) {
	return h.parent.HandlePayload(h.state, payload)
}

func (s *ConnectionState) markAuthenticated(diskID string) {
	s.mu.Lock()
	s.authenticatedDisk[diskID] = struct{}{}
	s.mu.Unlock()
}

func (s *ConnectionState) isAuthenticated(diskID string) bool {
	s.mu.RLock()
	_, ok := s.authenticatedDisk[diskID]
	s.mu.RUnlock()
	return ok
}
