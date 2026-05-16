package session

import (
	"time"

	filestorage "yumedisk/server/internal/storage/file"
)

type Service struct {
	manager      *Manager
	storage      *filestorage.Backend
	defaultTTL   time.Duration
	defaultMaxIO uint32
}

func NewService(manager *Manager, storage *filestorage.Backend, defaultTTL time.Duration, defaultMaxIO uint32) *Service {
	return &Service{
		manager:      manager,
		storage:      storage,
		defaultTTL:   defaultTTL,
		defaultMaxIO: defaultMaxIO,
	}
}

func (s *Service) Open(connectionID uint64, diskID string) Descriptor {
	return s.manager.Open(Descriptor{
		DiskID:     diskID,
		DiskSize:   s.storage.Size(),
		ReadOnly:   s.storage.ReadOnly(),
		MaxIOBytes: s.defaultMaxIO,
		ExpiresAt:  time.Now().Add(s.defaultTTL),
		Connection: connectionID,
	})
}

func (s *Service) Ping(sessionID uint64) (Descriptor, bool) {
	desc, ok := s.manager.Get(sessionID)
	if !ok {
		return Descriptor{}, false
	}
	if time.Now().After(desc.ExpiresAt) {
		s.manager.Close(sessionID)
		return Descriptor{}, false
	}

	desc.ExpiresAt = time.Now().Add(s.defaultTTL)
	s.manager.Update(desc)
	return desc, true
}

func (s *Service) Close(sessionID uint64) {
	s.manager.Close(sessionID)
}

func (s *Service) CloseConnection(connectionID uint64) {
	s.manager.CloseConnection(connectionID)
}

func (s *Service) TTLSeconds() uint32 {
	return uint32(s.defaultTTL / time.Second)
}

func (s *Service) Manager() *Manager {
	return s.manager
}
