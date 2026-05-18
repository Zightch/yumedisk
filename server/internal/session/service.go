package session

import (
	"errors"
	"fmt"
	"time"

	filestorage "yumedisk/server/internal/storage/file"
)

var (
	ErrSessionUnavailable = errors.New("session unavailable")
	ErrSessionBusy        = errors.New("session busy")
	ErrIOLimit            = errors.New("io limit exceeded")
	ErrOutOfRange         = errors.New("io out of range")
	ErrReadOnly           = errors.New("session is read only")
	ErrIOFailed           = errors.New("io failed")
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

func (s *Service) Open(connectionID uint64, diskID string) (Descriptor, error) {
	now := time.Now()
	desc, ok := s.manager.OpenExclusive(Descriptor{
		DiskID:     diskID,
		DiskSize:   s.storage.Size(),
		ReadOnly:   s.storage.ReadOnly(),
		MaxIOBytes: s.defaultMaxIO,
		TTLSeconds: s.TTLSeconds(),
		ExpiresAt:  now.Add(s.defaultTTL),
		Connection: connectionID,
	}, now)
	if !ok {
		return Descriptor{}, ErrSessionBusy
	}
	return desc, nil
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

func (s *Service) MaxIOBytes() uint32 {
	return s.defaultMaxIO
}

func (s *Service) Manager() *Manager {
	return s.manager
}

func (s *Service) Read(sessionID uint64, offset uint64, length uint32) ([]byte, error) {
	desc, err := s.touch(sessionID)
	if err != nil {
		return nil, err
	}
	if length == 0 || length > desc.MaxIOBytes {
		return nil, ErrIOLimit
	}
	if offset > desc.DiskSize || uint64(length) > desc.DiskSize-offset {
		return nil, ErrOutOfRange
	}

	data := make([]byte, length)
	if err := s.storage.ReadAt(offset, data); err != nil {
		return nil, mapStorageError(err)
	}
	return data, nil
}

func (s *Service) Write(sessionID uint64, offset uint64, data []byte) error {
	desc, err := s.touch(sessionID)
	if err != nil {
		return err
	}
	if desc.ReadOnly {
		return ErrReadOnly
	}
	if len(data) == 0 || uint32(len(data)) > desc.MaxIOBytes {
		return ErrIOLimit
	}
	if offset > desc.DiskSize || uint64(len(data)) > desc.DiskSize-offset {
		return ErrOutOfRange
	}

	if err := s.storage.WriteAt(offset, data); err != nil {
		return mapStorageError(err)
	}
	return nil
}

func (s *Service) validate(sessionID uint64) (Descriptor, error) {
	desc, ok := s.manager.Get(sessionID)
	if !ok {
		return Descriptor{}, ErrSessionUnavailable
	}
	if time.Now().After(desc.ExpiresAt) {
		s.manager.Close(sessionID)
		return Descriptor{}, ErrSessionUnavailable
	}
	return desc, nil
}

func (s *Service) touch(sessionID uint64) (Descriptor, error) {
	desc, err := s.validate(sessionID)
	if err != nil {
		return Descriptor{}, err
	}

	desc.ExpiresAt = time.Now().Add(s.defaultTTL)
	s.manager.Update(desc)
	return desc, nil
}

func mapStorageError(err error) error {
	switch {
	case errors.Is(err, filestorage.ErrReadOnly):
		return ErrReadOnly
	case errors.Is(err, filestorage.ErrOutOfRange):
		return ErrOutOfRange
	case errors.Is(err, filestorage.ErrIOFailed):
		return ErrIOFailed
	default:
		return fmt.Errorf("storage error: %w", err)
	}
}
