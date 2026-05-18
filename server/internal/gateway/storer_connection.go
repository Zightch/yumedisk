package gateway

import (
	"context"
	"errors"
	"fmt"
	"net"
	"sync"
	"sync/atomic"
	"time"

	"yumedisk/server/internal/proto"
	"yumedisk/server/internal/route"
	"yumedisk/server/internal/transport"
)

type storerConnection struct {
	id   uint64
	conn net.Conn

	writeMu sync.Mutex

	nextRequestID atomic.Uint64

	pendingMu sync.Mutex
	pending   map[uint64]chan []byte

	lastSeenUnixNano atomic.Int64
	heartbeatStop    chan struct{}
	heartbeatOnce    sync.Once
}

func newStorerConnection(id uint64, conn net.Conn) *storerConnection {
	return &storerConnection{
		id:            id,
		conn:          conn,
		pending:       make(map[uint64]chan []byte),
		heartbeatStop: make(chan struct{}),
	}
}

func (c *storerConnection) roundTrip(payload []byte) ([]byte, error) {
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

	resp, ok := <-respCh
	if !ok {
		return nil, errors.New("storer connection closed")
	}
	return resp, nil
}

func (c *storerConnection) serve(ctx context.Context, routes *route.Registry, gatewayToken string) error {
	buffer := make([]byte, transport.MaxPayloadSize)
	registered := false
	c.markSeen()
	defer c.stopHeartbeat()

	for {
		payload, err := transport.ReadFrameInto(c.conn, buffer)
		if err != nil {
			if errors.Is(err, net.ErrClosed) {
				return nil
			}
			return err
		}

		header, err := proto.ParseHeader(payload)
		if err != nil {
			return err
		}
		c.markSeen()
		if header.Flags&proto.FlagResponse == 0 {
			if header.OpCode != proto.OpStorerRegister {
				resp := proto.BuildErrorResponse(header, proto.StatusInvalidRequest)
				_ = transport.WriteFrame(c.conn, resp)
				continue
			}
			if registered {
				resp := proto.BuildErrorResponse(header, proto.StatusInvalidRequest)
				_ = transport.WriteFrame(c.conn, resp)
				continue
			}
			req, err := proto.ParseStorerRegisterRequestBody(payload[proto.HeaderSize:])
			if err != nil {
				resp := proto.BuildErrorResponse(header, proto.StatusBadBody)
				_ = transport.WriteFrame(c.conn, resp)
				continue
			}
			if req.GatewayToken != gatewayToken {
				resp := proto.BuildErrorResponse(header, proto.StatusAuthFailed)
				_ = transport.WriteFrame(c.conn, resp)
				continue
			}
			err = routes.Register(route.Entry{
				DiskID:        req.DiskID,
				AuthVerifier:  req.AuthVerifier,
				RouteTarget:   c.conn.RemoteAddr().String(),
				ConnectionID:  c.id,
				Connected:     true,
				DiskSizeBytes: req.DiskSizeBytes,
				ReadOnly:      req.ReadOnly,
				MaxIOBytes:    req.MaxIOBytes,
			})
			if err != nil {
				resp := proto.BuildErrorResponse(header, proto.StatusInvalidRequest)
				_ = transport.WriteFrame(c.conn, resp)
				continue
			}
			registered = true
			resp := proto.BuildSuccessResponse(header, nil)
			_ = transport.WriteFrame(c.conn, resp)
			continue
		}
		if !registered {
			return fmt.Errorf("unexpected storer response before register")
		}

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

		if ctx.Err() != nil {
			return nil
		}
	}
}

func (c *storerConnection) closePending() {
	c.stopHeartbeat()
	c.pendingMu.Lock()
	for id, ch := range c.pending {
		delete(c.pending, id)
		close(ch)
	}
	c.pendingMu.Unlock()
}

func (c *storerConnection) nextNonZeroRequestID() uint64 {
	id := c.nextRequestID.Add(1)
	if id == 0 {
		id = c.nextRequestID.Add(1)
	}
	return id
}

func (c *storerConnection) markSeen() {
	c.lastSeenUnixNano.Store(time.Now().UnixNano())
}

func (c *storerConnection) startHeartbeat(interval time.Duration, timeout time.Duration) {
	go func() {
		ticker := time.NewTicker(interval)
		defer ticker.Stop()
		var nonce uint64
		for {
			select {
			case <-ticker.C:
				lastSeen := time.Unix(0, c.lastSeenUnixNano.Load())
				if !lastSeen.IsZero() && time.Since(lastSeen) >= timeout {
					_ = c.conn.Close()
					return
				}
				if !lastSeen.IsZero() && time.Since(lastSeen) < interval {
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
				if _, err := c.roundTrip(payload); err != nil {
					_ = c.conn.Close()
					return
				}
			case <-c.heartbeatStop:
				return
			}
		}
	}()
}

func (c *storerConnection) stopHeartbeat() {
	c.heartbeatOnce.Do(func() {
		close(c.heartbeatStop)
	})
}
