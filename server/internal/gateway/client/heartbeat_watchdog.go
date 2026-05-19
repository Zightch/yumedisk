package client

import (
	"errors"
	"net"
	"sync"
	"sync/atomic"
	"time"
)

const clientHeartbeatTimeout = 15 * time.Second

var errConnHeartbeatTimeout = errors.New("client heartbeat timeout")

type clientHeartbeatWatchdog struct {
	timeout time.Duration

	deadlineUnixNano atomic.Int64
	stop             chan struct{}
	stopOnce         sync.Once
}

func newClientHeartbeatWatchdog(timeout time.Duration) *clientHeartbeatWatchdog {
	return &clientHeartbeatWatchdog{
		timeout: timeout,
		stop:    make(chan struct{}),
	}
}

func (w *clientHeartbeatWatchdog) Mark() {
	w.deadlineUnixNano.Store(time.Now().Add(w.timeout).UnixNano())
}

func (w *clientHeartbeatWatchdog) Start(conn net.Conn) <-chan error {
	errCh := make(chan error, 1)
	interval := w.timeout / 3
	if interval < 10*time.Millisecond {
		interval = 10 * time.Millisecond
	}

	go func() {
		ticker := time.NewTicker(interval)
		defer ticker.Stop()

		for {
			select {
			case <-ticker.C:
				deadline := w.deadlineUnixNano.Load()
				if deadline == 0 {
					continue
				}
				if time.Now().UnixNano() < deadline {
					continue
				}
				_ = conn.Close()
				errCh <- errConnHeartbeatTimeout
				return
			case <-w.stop:
				return
			}
		}
	}()

	return errCh
}

func (w *clientHeartbeatWatchdog) Stop() {
	w.stopOnce.Do(func() {
		close(w.stop)
	})
}
