package session

import (
	"sync"
	"sync/atomic"
)

type Manager interface {
	Open(record Record) (Record, error)
	Get(id uint64) (Record, bool)
	Close(id uint64)
	CloseConnection(connectionID uint64)
	beginIO(sessionID uint64) (Record, *ioLease, error)
}

type managedRecord struct {
	record   Record
	inFlight uint32
}

type managerState struct {
	nextID atomic.Uint64
	mu     sync.RWMutex
	items  map[uint64]managedRecord
}

type ioLease struct {
	releaseFn func()
	once      sync.Once
}

func newManagerState() managerState {
	return managerState{
		items: make(map[uint64]managedRecord),
	}
}

func (s *managerState) nextSessionIDLocked() uint64 {
	recordID := s.nextID.Add(1)
	if recordID == 0 {
		recordID = s.nextID.Add(1)
	}
	return recordID
}

func (s *managerState) storeLocked(record Record) Record {
	record.ID = s.nextSessionIDLocked()
	record.Closing = false
	s.items[record.ID] = managedRecord{record: record}
	return record
}

func (s *managerState) Get(id uint64) (Record, bool) {
	s.mu.RLock()
	defer s.mu.RUnlock()
	entry, ok := s.items[id]
	return entry.record, ok
}

func (s *managerState) Close(id uint64) {
	s.mu.Lock()
	s.closeLocked(id)
	s.mu.Unlock()
}

func (s *managerState) CloseConnection(connectionID uint64) {
	s.mu.Lock()
	for id, entry := range s.items {
		if entry.record.Connection == connectionID {
			s.closeLocked(id)
		}
	}
	s.mu.Unlock()
}

func (s *managerState) beginIO(sessionID uint64) (Record, *ioLease, error) {
	s.mu.Lock()
	defer s.mu.Unlock()

	entry, ok := s.items[sessionID]
	if !ok || entry.record.Closing {
		return Record{}, nil, ErrSessionUnavailable
	}
	entry.inFlight++
	s.items[sessionID] = entry
	return entry.record, &ioLease{
		releaseFn: func() {
			s.endIO(sessionID)
		},
	}, nil
}

func (s *managerState) endIO(sessionID uint64) {
	s.mu.Lock()
	entry, ok := s.items[sessionID]
	if !ok {
		s.mu.Unlock()
		return
	}
	if entry.inFlight > 0 {
		entry.inFlight--
	}
	if entry.record.Closing && entry.inFlight == 0 {
		delete(s.items, sessionID)
		s.mu.Unlock()
		return
	}
	s.items[sessionID] = entry
	s.mu.Unlock()
}

func (s *managerState) closeLocked(sessionID uint64) {
	entry, ok := s.items[sessionID]
	if !ok {
		return
	}
	entry.record.Closing = true
	if entry.inFlight == 0 {
		delete(s.items, sessionID)
		return
	}
	s.items[sessionID] = entry
}

func (l *ioLease) release() {
	if l == nil || l.releaseFn == nil {
		return
	}
	l.once.Do(l.releaseFn)
}
