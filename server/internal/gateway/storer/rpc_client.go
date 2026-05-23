package storer

import (
	"errors"
	"net"
	"sync"
	"sync/atomic"
	"time"

	"yumedisk/server/internal/proto"
	"yumedisk/server/internal/transport"
)

type connection struct {
	id   uint64
	conn net.Conn

	writeMu sync.Mutex

	nextRequestID atomic.Uint64
	registered    atomic.Bool

	pendingMu sync.Mutex
	pending   map[uint64]chan []byte

	heartbeatStop chan struct{}
	heartbeatOnce sync.Once
}

func newConnection(id uint64, conn net.Conn) *connection {
	return &connection{
		id:            id,
		conn:          conn,
		pending:       make(map[uint64]chan []byte),
		heartbeatStop: make(chan struct{}),
	}
}

func (c *connection) roundTrip(payload []byte) ([]byte, error) {
	return c.roundTripWithTimeout(payload, 0)
}

func (c *connection) notify(payload []byte) error {
	c.writeMu.Lock()
	err := transport.WriteFrame(c.conn, payload)
	c.writeMu.Unlock()
	return err
}

func (c *connection) roundTripWithTimeout(payload []byte, timeout time.Duration) ([]byte, error) {
	header, err := proto.ParseHeader(payload)
	if err != nil {
		return nil, err
	}
	header.RequestID = c.nextNonZeroRequestID()
	proto.EncodeHeader(header, payload)

	respCh := make(chan []byte, 1)
	c.pendingMu.Lock()
	c.pending[header.RequestID] = respCh
	c.pendingMu.Unlock()

	c.writeMu.Lock()
	err = transport.WriteFrame(c.conn, payload)
	c.writeMu.Unlock()
	if err != nil {
		c.pendingMu.Lock()
		delete(c.pending, header.RequestID)
		c.pendingMu.Unlock()
		return nil, err
	}

	if timeout <= 0 {
		resp, ok := <-respCh
		if !ok {
			return nil, errors.New("storer connection closed")
		}
		return resp, nil
	}

	timer := time.NewTimer(timeout)
	defer timer.Stop()

	select {
	case resp, ok := <-respCh:
		if !ok {
			return nil, errors.New("storer connection closed")
		}
		return resp, nil
	case <-timer.C:
		c.pendingMu.Lock()
		delete(c.pending, header.RequestID)
		c.pendingMu.Unlock()
		return nil, errors.New("storer heartbeat timeout")
	}
}

func (c *connection) handleResponse(header proto.Header, payload []byte) {
	resp := make([]byte, len(payload))
	copy(resp, payload)

	c.pendingMu.Lock()
	ch := c.pending[header.RequestID]
	delete(c.pending, header.RequestID)
	c.pendingMu.Unlock()
	if ch != nil {
		ch <- resp
		close(ch)
	}
}

func (c *connection) markRegistered() {
	c.registered.Store(true)
}

func (c *connection) closePending() {
	c.stopHeartbeat()
	c.pendingMu.Lock()
	for id, ch := range c.pending {
		delete(c.pending, id)
		close(ch)
	}
	c.pendingMu.Unlock()
}

func (c *connection) nextNonZeroRequestID() uint64 {
	id := c.nextRequestID.Add(1)
	if id == 0 {
		id = c.nextRequestID.Add(1)
	}
	return id
}

func (c *connection) startHeartbeat(interval time.Duration, timeout time.Duration) {
	go func() {
		ticker := time.NewTicker(interval)
		defer ticker.Stop()
		var nonce uint64
		for {
			select {
			case <-ticker.C:
				if !c.registered.Load() {
					continue
				}
				nonce++
				if nonce == 0 {
					nonce++
				}
				payload := make([]byte, proto.HeaderSize+proto.LinkHeartbeatBodySize)
				proto.EncodeHeader(proto.Header{
					ProtocolVersion: proto.ProtocolVersion,
					HeaderLen:       proto.HeaderSize,
					OpCode:          proto.OpLinkHeartbeat,
					RequestID:       1,
				}, payload)
				copy(payload[proto.HeaderSize:], proto.BuildLinkHeartbeatBody(nonce))
				if _, err := c.roundTripWithTimeout(payload, timeout); err != nil {
					_ = c.conn.Close()
					return
				}
			case <-c.heartbeatStop:
				return
			}
		}
	}()
}

func (c *connection) stopHeartbeat() {
	c.heartbeatOnce.Do(func() {
		close(c.heartbeatStop)
	})
}
