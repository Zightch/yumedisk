package session

import (
	"sync"
	"sync/atomic"
	"time"
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

func (m *Manager) Open(record Record) Record {
	record.ID = m.nextID.Add(1)
	if record.ID == 0 {
		record.ID = m.nextID.Add(1)
	}

	m.mu.Lock()
	m.items[record.ID] = record
	m.mu.Unlock()
	return record
}

func (m *Manager) OpenExclusive(record Record, now time.Time) (Record, bool) {
	m.mu.Lock()
	defer m.mu.Unlock()

	for id, existing := range m.items {
		if now.After(existing.ExpiresAt) {
			delete(m.items, id)
			continue
		}
		return Record{}, false
	}

	record.ID = m.nextID.Add(1)
	if record.ID == 0 {
		record.ID = m.nextID.Add(1)
	}
	m.items[record.ID] = record
	return record, true
}

func (m *Manager) Get(id uint64) (Record, bool) {
	m.mu.RLock()
	defer m.mu.RUnlock()
	record, ok := m.items[id]
	return record, ok
}

func (m *Manager) Update(record Record) {
	m.mu.Lock()
	m.items[record.ID] = record
	m.mu.Unlock()
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
