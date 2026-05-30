package session

type CloseNotifier interface {
	NotifySessionClosed(sessionID uint64, clientConnectionID uint64, reason uint16)
}

type DataChangedNotifier interface {
	NotifySessionDataChanged(sessionID uint64, clientConnectionID uint64)
}
