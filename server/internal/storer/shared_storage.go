package storer

import (
	filestorage "yumedisk/server/internal/storage/file"
)

type sharedStorage struct {
	backend *filestorage.Backend
}

func openSharedStorage(path string) (*sharedStorage, error) {
	backend, err := filestorage.Open(path, false)
	if err != nil {
		return nil, err
	}
	return &sharedStorage{backend: backend}, nil
}

func (s *sharedStorage) Close() error {
	if s == nil || s.backend == nil {
		return nil
	}
	return s.backend.Close()
}

func (s *sharedStorage) Path() string {
	return s.backend.Path()
}

func (s *sharedStorage) Size() uint64 {
	return s.backend.Size()
}
