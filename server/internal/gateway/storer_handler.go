package gateway

import (
	"context"
	"fmt"
)

type StorerHandler struct {
	routes *StorerRouteRegistry
}

func NewStorerHandler(routes *StorerRouteRegistry) (*StorerHandler, error) {
	if routes == nil {
		return nil, fmt.Errorf("storer handler requires route registry")
	}
	return &StorerHandler{routes: routes}, nil
}

func (h *StorerHandler) ServeConnection(conn *storerConnection, gatewayToken string) error {
	if conn == nil {
		return fmt.Errorf("storer handler requires connection")
	}
	return conn.serve(context.Background(), h.routes.routes, gatewayToken)
}
