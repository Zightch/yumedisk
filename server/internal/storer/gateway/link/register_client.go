package gateway

import (
	"fmt"
	"net"

	"yumedisk/server/internal/proto"
	"yumedisk/server/internal/transport"
)

type RegisterInfo struct {
	GatewayToken  string
	DiskID        string
	AuthVerifier  [64]byte
	DiskSizeBytes uint64
	ReadOnly      bool
	MaxIOBytes    uint32
}

type RegisterClient struct {
	request proto.StorerRegisterRequest
}

func NewRegisterClient(info RegisterInfo) *RegisterClient {
	return &RegisterClient{
		request: proto.StorerRegisterRequest{
			GatewayToken:  info.GatewayToken,
			DiskID:        info.DiskID,
			AuthVerifier:  info.AuthVerifier,
			DiskSizeBytes: info.DiskSizeBytes,
			ReadOnly:      info.ReadOnly,
			MaxIOBytes:    info.MaxIOBytes,
		},
	}
}

func (c *RegisterClient) Register(conn net.Conn) error {
	registerReq := proto.BuildStorerRegisterRequestBody(c.request)
	registerPayload := make([]byte, proto.HeaderSize+len(registerReq))
	proto.EncodeHeader(proto.Header{
		ProtocolVersion: proto.ProtocolVersion,
		HeaderLen:       proto.HeaderSize,
		OpCode:          proto.OpStorerRegister,
		RequestID:       1,
	}, registerPayload)
	copy(registerPayload[proto.HeaderSize:], registerReq)
	if err := transport.WriteFrame(conn, registerPayload); err != nil {
		return fmt.Errorf("write register request: %w", err)
	}

	buffer := make([]byte, transport.MaxPayloadSize)
	registerResp, err := transport.ReadFrameInto(conn, buffer)
	if err != nil {
		return fmt.Errorf("read register response: %w", err)
	}
	registerHeader, err := proto.ParseHeader(registerResp)
	if err != nil {
		return fmt.Errorf("parse register response: %w", err)
	}
	if registerHeader.StatusCode != proto.StatusOK {
		return fmt.Errorf("register rejected: status=%d", registerHeader.StatusCode)
	}
	return nil
}
