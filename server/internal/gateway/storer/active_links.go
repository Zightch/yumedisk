package storer

import "net"

type activeLinks struct {
	connections *connectionRegistry
	notifier    *disconnectNotifier
}

func newActiveLinks() *activeLinks {
	return &activeLinks{
		connections: newConnectionRegistry(),
		notifier:    newDisconnectNotifier(),
	}
}

func (l *activeLinks) SetDisconnectHandler(handler DisconnectHandler) {
	l.notifier.SetHandler(handler)
}

func (l *activeLinks) Attach(connectionID uint64, conn net.Conn) *connection {
	return l.connections.Attach(connectionID, conn)
}

func (l *activeLinks) Lookup(connectionID uint64) (*connection, bool) {
	return l.connections.Lookup(connectionID)
}

func (l *activeLinks) Disconnect(connectionID uint64, disconnectedDiskIDs []string) {
	conn := l.connections.Remove(connectionID)
	if conn != nil {
		conn.closePending()
	}
	l.notifier.Notify(connectionID, disconnectedDiskIDs)
}
