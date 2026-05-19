package storer

import "sync"

type disconnectNotifier struct {
	handlerMu sync.RWMutex
	handler   DisconnectHandler
}

func newDisconnectNotifier() *disconnectNotifier {
	return &disconnectNotifier{}
}

func (n *disconnectNotifier) SetHandler(handler DisconnectHandler) {
	n.handlerMu.Lock()
	n.handler = handler
	n.handlerMu.Unlock()
}

func (n *disconnectNotifier) Notify(connectionID uint64, disconnectedDiskIDs []string) {
	if len(disconnectedDiskIDs) == 0 {
		return
	}

	n.handlerMu.RLock()
	handler := n.handler
	n.handlerMu.RUnlock()
	if handler == nil {
		return
	}
	handler.CloseRouteConnection(connectionID, disconnectedDiskIDs)
}
