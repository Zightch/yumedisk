package session

type sharedManager struct {
	state managerState
}

func NewSharedManager() Manager {
	return &sharedManager{
		state: newManagerState(),
	}
}

func (m *sharedManager) Open(record Record) (Record, error) {
	m.state.mu.Lock()
	defer m.state.mu.Unlock()
	return m.state.storeLocked(record), nil
}

func (m *sharedManager) Get(id uint64) (Record, bool) {
	return m.state.Get(id)
}

func (m *sharedManager) Close(id uint64) {
	m.state.Close(id)
}

func (m *sharedManager) CloseConnection(connectionID uint64) {
	m.state.CloseConnection(connectionID)
}

func (m *sharedManager) beginIO(sessionID uint64) (Record, *ioLease, error) {
	return m.state.beginIO(sessionID)
}
