package storer

import (
	"yumedisk/server/internal/proto"
	"yumedisk/server/internal/route"
)

type registerGate struct {
	routes       *route.Registry
	gatewayToken string
}

func newRegisterGate(routes *route.Registry, gatewayToken string) *registerGate {
	return &registerGate{
		routes:       routes,
		gatewayToken: gatewayToken,
	}
}

func (g *registerGate) Handle(connectionID uint64, remoteAddr string, header proto.Header, body []byte) ([]byte, bool) {
	if header.OpCode != proto.OpStorerRegister {
		return proto.BuildErrorResponse(header, proto.StatusInvalidRequest), false
	}

	req, err := proto.ParseStorerRegisterRequestBody(body)
	if err != nil {
		return proto.BuildErrorResponse(header, proto.StatusBadBody), false
	}
	if req.GatewayToken != g.gatewayToken {
		return proto.BuildErrorResponse(header, proto.StatusAuthFailed), false
	}
	if err := g.routes.Register(route.Entry{
		DiskID:        req.DiskID,
		AuthVerifier:  req.AuthVerifier,
		RouteTarget:   remoteAddr,
		ConnectionID:  connectionID,
		Connected:     true,
		DiskSizeBytes: req.DiskSizeBytes,
		ReadOnly:      req.ReadOnly,
		MaxIOBytes:    req.MaxIOBytes,
	}); err != nil {
		return proto.BuildErrorResponse(header, proto.StatusInvalidRequest), false
	}
	return proto.BuildSuccessResponse(header, nil), true
}
