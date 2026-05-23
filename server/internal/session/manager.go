package session

import (
	"sync"
	"sync/atomic"
)

type Manager struct {
	nextID atomic.Uint64
	mu     sync.RWMutex
	items  map[uint64]managedRecord
}

type managedRecord struct {
	record   Record
	inFlight uint32
}

type ioLease struct {
	manager   *Manager
	sessionID uint64
	once      sync.Once
}

func NewManager() *Manager {
	return &Manager{
		items: make(map[uint64]managedRecord),
	}
}

func (m *Manager) Open(record Record) (Record, error) {
	m.mu.Lock()
	defer m.mu.Unlock()
	if len(m.items) > 0 {
		return Record{}, ErrSessionOpenRejected
	}

	record.ID = m.nextID.Add(1)
	if record.ID == 0 {
		record.ID = m.nextID.Add(1)
	}

	record.Closing = false
	m.items[record.ID] = managedRecord{record: record}
	return record, nil
}

func (m *Manager) Get(id uint64) (Record, bool) {
	m.mu.RLock()
	defer m.mu.RUnlock()
	entry, ok := m.items[id]
	return entry.record, ok
}

func (m *Manager) Close(id uint64) {
	m.mu.Lock()
	m.closeLocked(id)
	m.mu.Unlock()
}

func (m *Manager) CloseConnection(connectionID uint64) {
	m.mu.Lock()
	for id, entry := range m.items {
		if entry.record.Connection == connectionID {
			m.closeLocked(id)
		}
	}
	m.mu.Unlock()
}

func (m *Manager) beginIO(sessionID uint64) (Record, *ioLease, error) {
	m.mu.Lock()
	defer m.mu.Unlock()

	entry, ok := m.items[sessionID]
	if !ok || entry.record.Closing {
		return Record{}, nil, ErrSessionUnavailable
	}
	entry.inFlight++
	m.items[sessionID] = entry
	return entry.record, &ioLease{
		manager:   m,
		sessionID: sessionID,
	}, nil
}

func (m *Manager) endIO(sessionID uint64) {
	m.mu.Lock()
	entry, ok := m.items[sessionID]
	if !ok {
		m.mu.Unlock()
		return
	}
	if entry.inFlight > 0 {
		entry.inFlight--
	}
	if entry.record.Closing && entry.inFlight == 0 {
		delete(m.items, sessionID)
		m.mu.Unlock()
		return
	}
	m.items[sessionID] = entry
	m.mu.Unlock()
}

func (m *Manager) closeLocked(sessionID uint64) {
	entry, ok := m.items[sessionID]
	if !ok {
		return
	}
	entry.record.Closing = true
	if entry.inFlight == 0 {
		delete(m.items, sessionID)
		return
	}
	m.items[sessionID] = entry
}

func (l *ioLease) release() {
	if l == nil || l.manager == nil {
		return
	}
	l.once.Do(func() {
		l.manager.endIO(l.sessionID)
	})
}
