package gateway

import (
	"fmt"

	"yumedisk/server/internal/proto"
)

type Handler struct{}

type ConnectionState struct {
	ID uint64
}

type ConnectionHandler struct {
	parent *Handler
	state  *ConnectionState
}

func NewHandler() *Handler {
	return &Handler{}
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
	return proto.BuildErrorResponse(header, proto.StatusUnsupportedOp), nil
}

func (h *ConnectionHandler) HandlePayload(payload []byte) ([]byte, error) {
	return h.parent.HandlePayload(h.state, payload)
}
