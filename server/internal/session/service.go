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
	manager    *Manager
	storage    *filestorage.Backend
	metadata   Metadata
	defaultTTL time.Duration
}

func NewService(manager *Manager, storage *filestorage.Backend, metadata Metadata, defaultTTL time.Duration) *Service {
	return &Service{
		manager:    manager,
		storage:    storage,
		metadata:   metadata,
		defaultTTL: defaultTTL,
	}
}

func (s *Service) Open(connectionID uint64) (Record, error) {
	now := time.Now()
	record, ok := s.manager.OpenExclusive(Record{
		Connection: connectionID,
		Metadata:   s.metadata,
		ExpiresAt:  now.Add(s.defaultTTL),
	}, now)
	if !ok {
		return Record{}, ErrSessionBusy
	}
	return record, nil
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
	return s.metadata.MaxIOBytes
}

func (s *Service) Metadata() Metadata {
	return s.metadata
}

func (s *Service) Manager() *Manager {
	return s.manager
}

func (s *Service) Read(sessionID uint64, offset uint64, length uint32) ([]byte, error) {
	record, err := s.touch(sessionID)
	if err != nil {
		return nil, err
	}
	if length == 0 || length > record.Metadata.MaxIOBytes {
		return nil, ErrIOLimit
	}
	if offset > record.Metadata.DiskSizeBytes || uint64(length) > record.Metadata.DiskSizeBytes-offset {
		return nil, ErrOutOfRange
	}

	data := make([]byte, length)
	if err := s.storage.ReadAt(offset, data); err != nil {
		return nil, mapStorageError(err)
	}
	return data, nil
}

func (s *Service) Write(sessionID uint64, offset uint64, data []byte) error {
	record, err := s.touch(sessionID)
	if err != nil {
		return err
	}
	if record.Metadata.ReadOnly {
		return ErrReadOnly
	}
	if len(data) == 0 || uint32(len(data)) > record.Metadata.MaxIOBytes {
		return ErrIOLimit
	}
	if offset > record.Metadata.DiskSizeBytes || uint64(len(data)) > record.Metadata.DiskSizeBytes-offset {
		return ErrOutOfRange
	}

	if err := s.storage.WriteAt(offset, data); err != nil {
		return mapStorageError(err)
	}
	return nil
}

func (s *Service) validate(sessionID uint64) (Record, error) {
	record, ok := s.manager.Get(sessionID)
	if !ok {
		return Record{}, ErrSessionUnavailable
	}
	if time.Now().After(record.ExpiresAt) {
		s.manager.Close(sessionID)
		return Record{}, ErrSessionUnavailable
	}
	return record, nil
}

func (s *Service) touch(sessionID uint64) (Record, error) {
	record, err := s.validate(sessionID)
	if err != nil {
		return Record{}, err
	}

	record.ExpiresAt = time.Now().Add(s.defaultTTL)
	s.manager.Update(record)
	return record, nil
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
