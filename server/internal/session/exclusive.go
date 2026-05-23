package session

type exclusiveManager struct {
	state managerState
}

func NewExclusiveManager() Manager {
	return &exclusiveManager{
		state: newManagerState(),
	}
}

func (m *exclusiveManager) Open(record Record) (Record, error) {
	m.state.mu.Lock()
	defer m.state.mu.Unlock()
	if len(m.state.items) > 0 {
		return Record{}, ErrSessionOpenRejected
	}
	return m.state.storeLocked(record), nil
}

func (m *exclusiveManager) Get(id uint64) (Record, bool) {
	return m.state.Get(id)
}

func (m *exclusiveManager) Close(id uint64) {
	m.state.Close(id)
}

func (m *exclusiveManager) CloseConnection(connectionID uint64) {
	m.state.CloseConnection(connectionID)
}

func (m *exclusiveManager) beginIO(sessionID uint64) (Record, *ioLease, error) {
	return m.state.beginIO(sessionID)
}
