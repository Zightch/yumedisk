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
	"yumedisk/server/internal/session"
	"yumedisk/server/internal/transport"
)

type StorerRouteRegistry struct {
	routes *route.Registry

	handlerMu sync.RWMutex
	handler   routeDisconnectHandler

	mu          sync.RWMutex
	connections map[uint64]*storerConnection
}

type routeDisconnectHandler interface {
	CloseRouteConnection(routeConnectionID uint64, diskIDs []string)
}

func NewStorerRouteRegistry() *StorerRouteRegistry {
	return &StorerRouteRegistry{
		routes:      route.NewRegistry(),
		connections: make(map[uint64]*storerConnection),
	}
}

func (r *StorerRouteRegistry) SetDisconnectHandler(handler routeDisconnectHandler) {
	r.handlerMu.Lock()
	r.handler = handler
	r.handlerMu.Unlock()
}

func (r *StorerRouteRegistry) LookupRoute(diskID string) (route.Entry, bool) {
	return r.routes.LookupRoute(diskID)
}

func (r *StorerRouteRegistry) Register(entry route.Entry) error {
	return r.routes.Register(entry)
}

func (r *StorerRouteRegistry) AttachConnection(connectionID uint64, conn net.Conn) *storerConnection {
	sc := newStorerConnection(connectionID, conn)
	r.mu.Lock()
	r.connections[connectionID] = sc
	r.mu.Unlock()
	return sc
}

func (r *StorerRouteRegistry) DisconnectConnection(connectionID uint64) {
	disconnected := r.routes.DisconnectConnection(connectionID)

	r.mu.Lock()
	conn := r.connections[connectionID]
	delete(r.connections, connectionID)
	r.mu.Unlock()

	if conn != nil {
		conn.closePending()
	}

	r.handlerMu.RLock()
	handler := r.handler
	r.handlerMu.RUnlock()
	if handler != nil {
		diskIDs := make([]string, 0, len(disconnected))
		for _, entry := range disconnected {
			diskIDs = append(diskIDs, entry.DiskID)
		}
		handler.CloseRouteConnection(connectionID, diskIDs)
	}
}

func (r *StorerRouteRegistry) connection(connectionID uint64) (*storerConnection, bool) {
	r.mu.RLock()
	conn, ok := r.connections[connectionID]
	r.mu.RUnlock()
	return conn, ok
}

func (r *StorerRouteRegistry) Open(clientConnectionID uint64, entry route.Entry) (uint64, error) {
	conn, ok := r.connection(entry.ConnectionID)
	if !ok {
		return 0, session.ErrSessionUnavailable
	}

	req := make([]byte, proto.HeaderSize)
	proto.EncodeHeader(proto.Header{
		ProtocolVersion: proto.ProtocolVersion,
		HeaderLen:       proto.HeaderSize,
		OpCode:          proto.OpSessionOpen,
		RequestID:       1,
	}, req)

	resp, err := conn.roundTrip(req)
	if err != nil {
		return 0, session.ErrSessionUnavailable
	}
	header, err := proto.ParseHeader(resp)
	if err != nil {
		return 0, session.ErrSessionUnavailable
	}
	if header.StatusCode != proto.StatusOK {
		return 0, mapResponseStatus(header.StatusCode)
	}
	if len(resp) != proto.HeaderSize {
		return 0, session.ErrSessionUnavailable
	}
	return header.SessionID, nil
}

func (r *StorerRouteRegistry) Close(routeConnectionID uint64, sessionID uint64) {
	_, _ = r.roundTripData(routeConnectionID, proto.OpClose, sessionID, nil)
}

func (r *StorerRouteRegistry) CloseConnection(uint64) {}

func (r *StorerRouteRegistry) Read(routeConnectionID uint64, sessionID uint64, offset uint64, length uint32) ([]byte, error) {
	resp, err := r.roundTripData(routeConnectionID, proto.OpReadAt, sessionID, proto.BuildReadBody(offset, length))
	if err != nil {
		return nil, session.ErrSessionUnavailable
	}
	header, err := proto.ParseHeader(resp)
	if err != nil {
		return nil, session.ErrSessionUnavailable
	}
	if header.StatusCode != proto.StatusOK {
		return nil, mapResponseStatus(header.StatusCode)
	}
	data := make([]byte, len(resp)-proto.HeaderSize)
	copy(data, resp[proto.HeaderSize:])
	return data, nil
}

func (r *StorerRouteRegistry) Write(routeConnectionID uint64, sessionID uint64, offset uint64, data []byte) error {
	body := append(proto.BuildReadWriteBody(offset, uint32(len(data))), data...)
	resp, err := r.roundTripData(routeConnectionID, proto.OpWriteAt, sessionID, body)
	if err != nil {
		return session.ErrSessionUnavailable
	}
	header, err := proto.ParseHeader(resp)
	if err != nil {
		return session.ErrSessionUnavailable
	}
	if header.StatusCode != proto.StatusOK {
		return mapResponseStatus(header.StatusCode)
	}
	return nil
}

func (r *StorerRouteRegistry) roundTripData(routeConnectionID uint64, opCode uint8, sessionID uint64, body []byte) ([]byte, error) {
	conn, ok := r.connection(routeConnectionID)
	if !ok {
		return nil, session.ErrSessionUnavailable
	}

	req := make([]byte, proto.HeaderSize+len(body))
	proto.EncodeHeader(proto.Header{
		ProtocolVersion: proto.ProtocolVersion,
		HeaderLen:       proto.HeaderSize,
		OpCode:          opCode,
		RequestID:       1,
		SessionID:       sessionID,
	}, req)
	copy(req[proto.HeaderSize:], body)
	return conn.roundTrip(req)
}

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

func (c *storerConnection) serve(ctx context.Context, registry *StorerRouteRegistry, gatewayToken string) error {
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
			err = registry.Register(route.Entry{
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

func mapResponseStatus(status uint16) error {
	switch status {
	case proto.StatusSessionBusy:
		return session.ErrSessionBusy
	case proto.StatusIOReadOnly:
		return session.ErrReadOnly
	case proto.StatusIOLarge:
		return session.ErrIOLimit
	case proto.StatusIOOutOfRange:
		return session.ErrOutOfRange
	case proto.StatusIOFailed:
		return session.ErrIOFailed
	case proto.StatusSessionUnavailable:
		return session.ErrSessionUnavailable
	default:
		return session.ErrSessionUnavailable
	}
}
