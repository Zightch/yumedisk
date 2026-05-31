package session

type CloseNotifier interface {
	NotifySessionClosed(sessionID uint64, clientConnectionID uint64, body []byte)
}

type DataChangedNotifier interface {
	NotifySessionDataChanged(sessionID uint64, clientConnectionID uint64)
}
