package gateway

import (
	"strings"
	"testing"
)

func TestStorerHandlerReturnsExplicitNotImplemented(t *testing.T) {
	t.Parallel()

	handler, err := NewStorerHandler(NewStorerRouteRegistry())
	if err != nil {
		t.Fatalf("new storer handler: %v", err)
	}

	_, err = handler.HandlePayload([]byte{1, 2, 3})
	if err == nil {
		t.Fatal("expected storer handler error")
	}
	if !strings.Contains(err.Error(), "not implemented") {
		t.Fatalf("unexpected storer handler error: %v", err)
	}
}
