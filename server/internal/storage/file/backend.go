package filestorage

import (
	"errors"
	"fmt"
	"io"
	"os"
	"path/filepath"
	"sync"
)

var (
	ErrReadOnly   = errors.New("storage backend is read only")
	ErrOutOfRange = errors.New("storage access out of range")
	ErrIOFailed   = errors.New("storage io failed")
)

type Backend struct {
	mu       sync.Mutex
	file     *os.File
	path     string
	readOnly bool
	size     uint64
}

func Open(path string, readOnly bool) (*Backend, error) {
	if !readOnly {
		if err := os.MkdirAll(filepath.Dir(path), 0o755); err != nil {
			return nil, fmt.Errorf("create storage directory: %w", err)
		}
	}

	flags := os.O_RDONLY
	if !readOnly {
		flags = os.O_CREATE | os.O_RDWR
	}

	file, err := os.OpenFile(path, flags, 0o644)
	if err != nil {
		if readOnly {
			return nil, fmt.Errorf("open read-only storage file: %w", err)
		}
		return nil, fmt.Errorf("open storage file: %w", err)
	}

	info, err := file.Stat()
	if err != nil {
		_ = file.Close()
		return nil, fmt.Errorf("stat storage file: %w", err)
	}

	return &Backend{
		file:     file,
		path:     path,
		readOnly: readOnly,
		size:     uint64(info.Size()),
	}, nil
}

func (b *Backend) Close() error {
	b.mu.Lock()
	defer b.mu.Unlock()

	if b.file == nil {
		return nil
	}

	err := b.file.Close()
	b.file = nil
	return err
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

func (b *Backend) ReadAt(offset uint64, dst []byte) error {
	if len(dst) == 0 {
		return nil
	}
	if err := b.checkRange(offset, uint64(len(dst))); err != nil {
		return err
	}

	b.mu.Lock()
	defer b.mu.Unlock()

	if b.file == nil {
		return ErrIOFailed
	}

	n, err := b.file.ReadAt(dst, int64(offset))
	if err != nil && !errors.Is(err, io.EOF) {
		return fmt.Errorf("read raw storage: %w", err)
	}
	if n != len(dst) {
		return ErrIOFailed
	}
	return nil
}

func (b *Backend) WriteAt(offset uint64, src []byte) error {
	if len(src) == 0 {
		return nil
	}
	if b.readOnly {
		return ErrReadOnly
	}
	if err := b.checkRange(offset, uint64(len(src))); err != nil {
		return err
	}

	b.mu.Lock()
	defer b.mu.Unlock()

	if b.file == nil {
		return ErrIOFailed
	}

	n, err := b.file.WriteAt(src, int64(offset))
	if err != nil {
		return fmt.Errorf("write raw storage: %w", err)
	}
	if n != len(src) {
		return ErrIOFailed
	}
	return nil
}

func (b *Backend) checkRange(offset, length uint64) error {
	if length == 0 {
		return nil
	}
	if offset > b.size {
		return ErrOutOfRange
	}
	if length > b.size-offset {
		return ErrOutOfRange
	}
	return nil
}
