package gateway

import "fmt"

type StorerHandler struct {
	routes *StorerRouteRegistry
}

func NewStorerHandler(routes *StorerRouteRegistry) (*StorerHandler, error) {
	if routes == nil {
		return nil, fmt.Errorf("storer handler requires route registry")
	}
	return &StorerHandler{routes: routes}, nil
}

func (h *StorerHandler) HandlePayload([]byte) ([]byte, error) {
	return nil, fmt.Errorf("storer-facing gateway protocol not implemented yet")
}
