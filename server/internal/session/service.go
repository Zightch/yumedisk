package session

import (
	"errors"
	"fmt"

	filestorage "yumedisk/server/internal/storage/file"
)

var (
	ErrSessionUnavailable  = errors.New("session unavailable")
	ErrSessionOpenRejected = errors.New("session open rejected")
	ErrIOLimit             = errors.New("io limit exceeded")
	ErrOutOfRange          = errors.New("io out of range")
	ErrReadOnly            = errors.New("session is read only")
	ErrIOFailed            = errors.New("io failed")
)

type Service struct {
	manager  Manager
	storage  *filestorage.Backend
	metadata Metadata

	writeCommittedHook func(Record)
}

func NewService(manager Manager, storage *filestorage.Backend, metadata Metadata) *Service {
	return &Service{
		manager:  manager,
		storage:  storage,
		metadata: metadata,
	}
}

func (s *Service) Open(connectionID uint64) (Record, error) {
	record, err := s.manager.Open(Record{
		Connection: connectionID,
		Metadata:   s.metadata,
	})
	if err != nil {
		return Record{}, err
	}
	return record, nil
}

func (s *Service) Close(sessionID uint64) {
	s.manager.Close(sessionID)
}

func (s *Service) CloseConnection(connectionID uint64) {
	s.manager.CloseConnection(connectionID)
}

func (s *Service) MaxIOBytes() uint32 {
	return s.metadata.MaxIOBytes
}

func (s *Service) Describe(sessionID uint64) (Metadata, error) {
	record, ok := s.manager.Get(sessionID)
	if !ok || record.Closing {
		return Metadata{}, ErrSessionUnavailable
	}
	return record.Metadata, nil
}

func (s *Service) Metadata() Metadata {
	return s.metadata
}

func (s *Service) LiveSessionIDs() []uint64 {
	records := s.manager.List()
	sessionIDs := make([]uint64, 0, len(records))
	for _, record := range records {
		if record.Closing {
			continue
		}
		sessionIDs = append(sessionIDs, record.ID)
	}
	return sessionIDs
}

func (s *Service) SetWriteCommittedHook(hook func(Record)) {
	s.writeCommittedHook = hook
}

func (s *Service) Manager() Manager {
	return s.manager
}

func (s *Service) Read(sessionID uint64, offset uint64, length uint32) ([]byte, error) {
	record, lease, err := s.manager.beginIO(sessionID)
	if err != nil {
		return nil, err
	}
	defer lease.release()
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
	record, lease, err := s.manager.beginIO(sessionID)
	if err != nil {
		return err
	}
	defer lease.release()
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
	if s.writeCommittedHook != nil {
		s.writeCommittedHook(record)
	}
	return nil
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
