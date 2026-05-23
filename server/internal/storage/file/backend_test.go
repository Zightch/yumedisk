package filestorage

import (
	"errors"
	"os"
	"path/filepath"
	"sync"
	"sync/atomic"
	"testing"
	"time"
)

func TestOpenRawBackendProbesSizeAndReadsWrites(t *testing.T) {
	t.Parallel()

	tempDir := t.TempDir()
	path := filepath.Join(tempDir, "disk.raw")
	initial := []byte("0123456789")
	if err := os.WriteFile(path, initial, 0o644); err != nil {
		t.Fatalf("write raw file: %v", err)
	}

	backend, err := Open(path, false)
	if err != nil {
		t.Fatalf("open backend: %v", err)
	}
	t.Cleanup(func() { _ = backend.Close() })

	if got := backend.Size(); got != uint64(len(initial)) {
		t.Fatalf("size mismatch: got %d want %d", got, len(initial))
	}

	buf := make([]byte, 4)
	if err := backend.ReadAt(2, buf); err != nil {
		t.Fatalf("read at: %v", err)
	}
	if string(buf) != "2345" {
		t.Fatalf("read mismatch: %q", string(buf))
	}

	if err := backend.WriteAt(5, []byte("AB")); err != nil {
		t.Fatalf("write at: %v", err)
	}

	updated, err := os.ReadFile(path)
	if err != nil {
		t.Fatalf("read updated file: %v", err)
	}
	if string(updated) != "01234AB789" {
		t.Fatalf("updated content mismatch: %q", string(updated))
	}
}

func TestRawBackendRejectsOutOfRangeAndReadOnlyWrites(t *testing.T) {
	t.Parallel()

	tempDir := t.TempDir()
	path := filepath.Join(tempDir, "disk.raw")
	if err := os.WriteFile(path, []byte("0123456789"), 0o644); err != nil {
		t.Fatalf("write raw file: %v", err)
	}

	backend, err := Open(path, true)
	if err != nil {
		t.Fatalf("open read-only backend: %v", err)
	}
	t.Cleanup(func() { _ = backend.Close() })

	if err := backend.ReadAt(9, make([]byte, 2)); !errors.Is(err, ErrOutOfRange) {
		t.Fatalf("expected out-of-range error, got %v", err)
	}
	if err := backend.WriteAt(0, []byte("X")); !errors.Is(err, ErrReadOnly) {
		t.Fatalf("expected read-only error, got %v", err)
	}
}

func TestBackendReadReadCanRunInParallel(t *testing.T) {
	t.Parallel()

	file := newBlockingTestFile()
	backend := newTestBackend(file)

	var wg sync.WaitGroup
	read := func() {
		defer wg.Done()
		if err := backend.ReadAt(0, make([]byte, 8)); err != nil {
			t.Errorf("read failed: %v", err)
		}
	}

	wg.Add(2)
	go read()
	go read()

	file.waitReadStarts(t, 2)
	if got := file.maxReads(); got != 2 {
		t.Fatalf("expected 2 concurrent reads, got %d", got)
	}

	file.releaseReads(2)
	wg.Wait()
}

func TestBackendWriteBlocksWhileReadInProgress(t *testing.T) {
	t.Parallel()

	file := newBlockingTestFile()
	backend := newTestBackend(file)

	readDone := make(chan error, 1)
	go func() {
		readDone <- backend.ReadAt(0, make([]byte, 8))
	}()
	file.waitReadStarts(t, 1)

	writeDone := make(chan error, 1)
	go func() {
		writeDone <- backend.WriteAt(0, []byte("ABCD"))
	}()

	file.assertNoWriteStart(t)
	file.releaseReads(1)
	if err := <-readDone; err != nil {
		t.Fatalf("read failed: %v", err)
	}

	file.waitWriteStarts(t, 1)
	file.releaseWrites(1)
	if err := <-writeDone; err != nil {
		t.Fatalf("write failed: %v", err)
	}
}

func TestBackendWriteWriteAreMutuallyExclusive(t *testing.T) {
	t.Parallel()

	file := newBlockingTestFile()
	backend := newTestBackend(file)

	firstDone := make(chan error, 1)
	go func() {
		firstDone <- backend.WriteAt(0, []byte("ABCD"))
	}()
	file.waitWriteStarts(t, 1)

	secondDone := make(chan error, 1)
	go func() {
		secondDone <- backend.WriteAt(4, []byte("EFGH"))
	}()

	file.assertNoWriteStart(t)
	file.releaseWrites(1)
	if err := <-firstDone; err != nil {
		t.Fatalf("first write failed: %v", err)
	}

	file.waitWriteStarts(t, 1)
	if got := file.maxWrites(); got != 1 {
		t.Fatalf("expected writes to stay mutually exclusive, got %d concurrent writes", got)
	}
	file.releaseWrites(1)
	if err := <-secondDone; err != nil {
		t.Fatalf("second write failed: %v", err)
	}
}

func TestBackendCloseWaitsForInflightAccess(t *testing.T) {
	t.Parallel()

	file := newBlockingTestFile()
	backend := newTestBackend(file)

	readDone := make(chan error, 1)
	go func() {
		readDone <- backend.ReadAt(0, make([]byte, 8))
	}()
	file.waitReadStarts(t, 1)

	closeDone := make(chan error, 1)
	go func() {
		closeDone <- backend.Close()
	}()

	select {
	case err := <-closeDone:
		t.Fatalf("close returned before inflight read drained: %v", err)
	case <-time.After(100 * time.Millisecond):
	}

	file.releaseReads(1)
	if err := <-readDone; err != nil {
		t.Fatalf("read failed: %v", err)
	}
	if err := <-closeDone; err != nil {
		t.Fatalf("close failed: %v", err)
	}
	if file.closeCount() != 1 {
		t.Fatalf("expected underlying file to close once, got %d", file.closeCount())
	}
	if err := backend.ReadAt(0, make([]byte, 1)); !errors.Is(err, ErrIOFailed) {
		t.Fatalf("expected io failed after close, got %v", err)
	}
}

func newTestBackend(file *blockingTestFile) *Backend {
	return &Backend{
		file: file,
		path: "test.raw",
		size: 4096,
	}
}

type blockingTestFile struct {
	readPermit         chan struct{}
	writePermit        chan struct{}
	readStarted        chan struct{}
	writeStarted       chan struct{}
	activeReads        int32
	activeWrites       int32
	maxConcurrentReads int32
	maxConcurrentWrite int32
	closeCalls         int32
}

func newBlockingTestFile() *blockingTestFile {
	return &blockingTestFile{
		readPermit:   make(chan struct{}),
		writePermit:  make(chan struct{}),
		readStarted:  make(chan struct{}, 8),
		writeStarted: make(chan struct{}, 8),
	}
}

func (f *blockingTestFile) ReadAt(dst []byte, _ int64) (int, error) {
	current := atomic.AddInt32(&f.activeReads, 1)
	f.updateMax(&f.maxConcurrentReads, current)
	f.readStarted <- struct{}{}
	<-f.readPermit
	atomic.AddInt32(&f.activeReads, -1)
	clear(dst)
	return len(dst), nil
}

func (f *blockingTestFile) WriteAt(src []byte, _ int64) (int, error) {
	current := atomic.AddInt32(&f.activeWrites, 1)
	f.updateMax(&f.maxConcurrentWrite, current)
	f.writeStarted <- struct{}{}
	<-f.writePermit
	atomic.AddInt32(&f.activeWrites, -1)
	return len(src), nil
}

func (f *blockingTestFile) Close() error {
	atomic.AddInt32(&f.closeCalls, 1)
	return nil
}

func (f *blockingTestFile) releaseReads(count int) {
	for i := 0; i < count; i++ {
		f.readPermit <- struct{}{}
	}
}

func (f *blockingTestFile) releaseWrites(count int) {
	for i := 0; i < count; i++ {
		f.writePermit <- struct{}{}
	}
}

func (f *blockingTestFile) waitReadStarts(t *testing.T, count int) {
	t.Helper()
	for i := 0; i < count; i++ {
		select {
		case <-f.readStarted:
		case <-time.After(2 * time.Second):
			t.Fatal("timed out waiting for read start")
		}
	}
}

func (f *blockingTestFile) waitWriteStarts(t *testing.T, count int) {
	t.Helper()
	for i := 0; i < count; i++ {
		select {
		case <-f.writeStarted:
		case <-time.After(2 * time.Second):
			t.Fatal("timed out waiting for write start")
		}
	}
}

func (f *blockingTestFile) assertNoWriteStart(t *testing.T) {
	t.Helper()
	select {
	case <-f.writeStarted:
		t.Fatal("write started unexpectedly while previous access should still hold lock")
	case <-time.After(100 * time.Millisecond):
	}
}

func (f *blockingTestFile) maxReads() int32 {
	return atomic.LoadInt32(&f.maxConcurrentReads)
}

func (f *blockingTestFile) maxWrites() int32 {
	return atomic.LoadInt32(&f.maxConcurrentWrite)
}

func (f *blockingTestFile) closeCount() int32 {
	return atomic.LoadInt32(&f.closeCalls)
}

func (f *blockingTestFile) updateMax(target *int32, current int32) {
	for {
		existing := atomic.LoadInt32(target)
		if existing >= current {
			return
		}
		if atomic.CompareAndSwapInt32(target, existing, current) {
			return
		}
	}
}
