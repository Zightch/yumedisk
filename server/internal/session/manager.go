package session

import (
	"sync"
	"sync/atomic"
)

type Manager struct {
	nextID atomic.Uint64
	mu     sync.RWMutex
	items  map[uint64]Record
}

func NewManager() *Manager {
	return &Manager{
		items: make(map[uint64]Record),
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

	m.items[record.ID] = record
	return record, nil
}

func (m *Manager) Get(id uint64) (Record, bool) {
	m.mu.RLock()
	defer m.mu.RUnlock()
	record, ok := m.items[id]
	return record, ok
}

func (m *Manager) Close(id uint64) {
	m.mu.Lock()
	delete(m.items, id)
	m.mu.Unlock()
}

func (m *Manager) CloseConnection(connectionID uint64) {
	m.mu.Lock()
	for id, record := range m.items {
		if record.Connection == connectionID {
			delete(m.items, id)
		}
	}
	m.mu.Unlock()
}
