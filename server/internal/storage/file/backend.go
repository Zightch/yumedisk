package filestorage

import (
	"fmt"
	"os"
	"path/filepath"
)

type Backend struct {
	path     string
	readOnly bool
	size     uint64
}

func Open(path string, readOnly bool) (*Backend, error) {
	if readOnly {
		file, err := os.Open(path)
		if err != nil {
			return nil, fmt.Errorf("open read-only storage file: %w", err)
		}
		defer file.Close()

		info, err := file.Stat()
		if err != nil {
			return nil, fmt.Errorf("stat read-only storage file: %w", err)
		}
		return &Backend{path: path, readOnly: true, size: uint64(info.Size())}, nil
	}

	if err := os.MkdirAll(filepath.Dir(path), 0o755); err != nil {
		return nil, fmt.Errorf("create storage directory: %w", err)
	}

	file, err := os.OpenFile(path, os.O_CREATE|os.O_RDWR, 0o644)
	if err != nil {
		return nil, fmt.Errorf("open storage file: %w", err)
	}
	defer file.Close()

	info, err := file.Stat()
	if err != nil {
		return nil, fmt.Errorf("stat storage file: %w", err)
	}
	return &Backend{path: path, readOnly: false, size: uint64(info.Size())}, nil
}

func (b *Backend) Path() string {
	return b.path
}

func (b *Backend) ReadOnly() bool {
	return b.readOnly
}

func (b *Backend) Size() uint64 {
	return b.size
}
