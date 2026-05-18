package gateway

type sessionCloseNotifier interface {
	NotifySessionClosed(session gatewaySessionRecord, reason uint16)
}
