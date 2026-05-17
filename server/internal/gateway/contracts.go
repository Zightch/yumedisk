package gateway

import "yumedisk/server/internal/session"

type AuthVerifierSource interface {
	LookupAuthVerifier(diskID string) ([64]byte, bool)
}

type SessionDataPlane interface {
	Open(connectionID uint64, diskID string) (session.Descriptor, error)
	Ping(sessionID uint64) (session.Descriptor, bool)
	Close(sessionID uint64)
	CloseConnection(connectionID uint64)
	Read(sessionID uint64, offset uint64, length uint32) ([]byte, error)
	Write(sessionID uint64, offset uint64, data []byte) error
	TTLSeconds() uint32
}
