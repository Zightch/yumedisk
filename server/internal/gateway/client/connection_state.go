package client

import (
	"errors"
	"sync"
)

type ConnectionState struct {
	ID uint64

	mu                sync.RWMutex
	authInFlight      bool
	openInFlight      bool
	heartbeatWatchdog *clientHeartbeatWatchdog
}

type ConnectionHandler struct {
	parent *Handler
	state  *ConnectionState
}

func (s *ConnectionState) ConnectionID() uint64 {
	return s.ID
}

func (s *ConnectionState) BeginSessionOpen() error {
	return s.beginSessionOpen()
}

func (s *ConnectionState) FinishSessionOpen() error {
	return s.finishSessionOpen()
}

func (s *ConnectionState) FailSessionOpen() {
	s.failSessionOpen()
}

func (s *ConnectionState) MarkHeartbeat() {
	s.markHeartbeat()
}

func (s *ConnectionState) beginAuth() error {
	s.mu.Lock()
	defer s.mu.Unlock()
	if s.authInFlight || s.openInFlight {
		return errPhaseViolation
	}
	s.authInFlight = true
	return nil
}

func (s *ConnectionState) finishAuth() error {
	s.mu.Lock()
	defer s.mu.Unlock()
	if !s.authInFlight || s.openInFlight {
		return errPhaseViolation
	}
	s.authInFlight = false
	return nil
}

func (s *ConnectionState) failAuth() {
	s.mu.Lock()
	s.authInFlight = false
	s.mu.Unlock()
}

func (s *ConnectionState) beginSessionOpen() error {
	s.mu.Lock()
	defer s.mu.Unlock()
	if s.authInFlight || s.openInFlight {
		return errPhaseViolation
	}
	s.openInFlight = true
	return nil
}

func (s *ConnectionState) finishSessionOpen() error {
	s.mu.Lock()
	defer s.mu.Unlock()
	if !s.openInFlight || s.authInFlight {
		return errPhaseViolation
	}
	s.openInFlight = false
	return nil
}

func (s *ConnectionState) failSessionOpen() {
	s.mu.Lock()
	s.openInFlight = false
	s.mu.Unlock()
}

func (s *ConnectionState) pendingAuth() bool {
	s.mu.RLock()
	pending := s.authInFlight
	s.mu.RUnlock()
	return pending
}

func (s *ConnectionState) setHeartbeatWatchdog(watchdog *clientHeartbeatWatchdog) {
	s.mu.Lock()
	s.heartbeatWatchdog = watchdog
	s.mu.Unlock()
}

func (s *ConnectionState) markHeartbeat() {
	s.mu.RLock()
	watchdog := s.heartbeatWatchdog
	s.mu.RUnlock()
	if watchdog != nil {
		watchdog.Mark()
	}
}

var (
	errPhaseViolation = errors.New("connection phase violation")
)
