package session

import (
	"sync"
	"sync/atomic"
	"time"
)

type Descriptor struct {
	ID         uint64
	DiskID     string
	DiskSize   uint64
	ReadOnly   bool
	MaxIOBytes uint32
	ExpiresAt  time.Time
	Connection uint64
}

type Manager struct {
	nextID atomic.Uint64
	mu     sync.RWMutex
	items  map[uint64]Descriptor
}

func NewManager() *Manager {
	return &Manager{
		items: make(map[uint64]Descriptor),
	}
}

func (m *Manager) Open(desc Descriptor) Descriptor {
	desc.ID = m.nextID.Add(1)
	if desc.ID == 0 {
		desc.ID = m.nextID.Add(1)
	}

	m.mu.Lock()
	m.items[desc.ID] = desc
	m.mu.Unlock()
	return desc
}

func (m *Manager) OpenExclusive(desc Descriptor, now time.Time) (Descriptor, bool) {
	m.mu.Lock()
	defer m.mu.Unlock()

	for id, existing := range m.items {
		if existing.DiskID != desc.DiskID {
			continue
		}
		if now.After(existing.ExpiresAt) {
			delete(m.items, id)
			continue
		}
		return Descriptor{}, false
	}

	desc.ID = m.nextID.Add(1)
	if desc.ID == 0 {
		desc.ID = m.nextID.Add(1)
	}
	m.items[desc.ID] = desc
	return desc, true
}

func (m *Manager) Get(id uint64) (Descriptor, bool) {
	m.mu.RLock()
	defer m.mu.RUnlock()
	desc, ok := m.items[id]
	return desc, ok
}

func (m *Manager) Update(desc Descriptor) {
	m.mu.Lock()
	m.items[desc.ID] = desc
	m.mu.Unlock()
}

func (m *Manager) Close(id uint64) {
	m.mu.Lock()
	delete(m.items, id)
	m.mu.Unlock()
}

func (m *Manager) CloseConnection(connectionID uint64) {
	m.mu.Lock()
	for id, desc := range m.items {
		if desc.Connection == connectionID {
			delete(m.items, id)
		}
	}
	m.mu.Unlock()
}
