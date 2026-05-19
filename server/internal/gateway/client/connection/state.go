package connection

import (
	"errors"
	"sync"
)

type HeartbeatMarker interface {
	Mark()
}

type State struct {
	ID uint64

	mu              sync.RWMutex
	authInFlight    bool
	openInFlight    bool
	heartbeatMarker HeartbeatMarker
}

func (s *State) ConnectionID() uint64 {
	return s.ID
}

func (s *State) BeginAuth() error {
	s.mu.Lock()
	defer s.mu.Unlock()
	if s.authInFlight || s.openInFlight {
		return errPhaseViolation
	}
	s.authInFlight = true
	return nil
}

func (s *State) FinishAuth() error {
	s.mu.Lock()
	defer s.mu.Unlock()
	if !s.authInFlight || s.openInFlight {
		return errPhaseViolation
	}
	s.authInFlight = false
	return nil
}

func (s *State) FailAuth() {
	s.mu.Lock()
	s.authInFlight = false
	s.mu.Unlock()
}

func (s *State) PendingAuth() bool {
	s.mu.RLock()
	pending := s.authInFlight
	s.mu.RUnlock()
	return pending
}

func (s *State) BeginSessionOpen() error {
	s.mu.Lock()
	defer s.mu.Unlock()
	if s.authInFlight || s.openInFlight {
		return errPhaseViolation
	}
	s.openInFlight = true
	return nil
}

func (s *State) FinishSessionOpen() error {
	s.mu.Lock()
	defer s.mu.Unlock()
	if !s.openInFlight || s.authInFlight {
		return errPhaseViolation
	}
	s.openInFlight = false
	return nil
}

func (s *State) FailSessionOpen() {
	s.mu.Lock()
	s.openInFlight = false
	s.mu.Unlock()
}

func (s *State) SetHeartbeatMarker(marker HeartbeatMarker) {
	s.mu.Lock()
	s.heartbeatMarker = marker
	s.mu.Unlock()
}

func (s *State) MarkHeartbeat() {
	s.mu.RLock()
	marker := s.heartbeatMarker
	s.mu.RUnlock()
	if marker != nil {
		marker.Mark()
	}
}

var errPhaseViolation = errors.New("connection phase violation")
