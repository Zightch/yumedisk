package storer

import (
	"errors"
	"net"
	"sync"
	"sync/atomic"
	"time"
)

var errLinkHeartbeatTimeout = errors.New("gateway heartbeat timeout")

type linkHeartbeatWatchdog struct {
	timeout time.Duration

	deadlineUnixNano atomic.Int64
	stop             chan struct{}
	stopOnce         sync.Once
}

func newLinkHeartbeatWatchdog(timeout time.Duration) *linkHeartbeatWatchdog {
	return &linkHeartbeatWatchdog{
		timeout: timeout,
		stop:    make(chan struct{}),
	}
}

func (w *linkHeartbeatWatchdog) Mark() {
	w.deadlineUnixNano.Store(time.Now().Add(w.timeout).UnixNano())
}

func (w *linkHeartbeatWatchdog) Start(conn net.Conn) <-chan error {
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
				errCh <- errLinkHeartbeatTimeout
				return
			case <-w.stop:
				return
			}
		}
	}()

	return errCh
}

func (w *linkHeartbeatWatchdog) Stop() {
	w.stopOnce.Do(func() {
		close(w.stop)
	})
}
