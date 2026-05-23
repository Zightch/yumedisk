package storer

import (
	"yumedisk/server/internal/config"
	"yumedisk/server/internal/route"
	"yumedisk/server/internal/session"
	gatewaylink "yumedisk/server/internal/storer/gateway/link"
)

type Core struct {
	disk     *localDisk
	sessions *session.Service
}

func NewCore(cfg config.StorerConfig) (*Core, error) {
	disk, err := openLocalDisk(cfg)
	if err != nil {
		return nil, err
	}

	return &Core{
		disk:     disk,
		sessions: session.NewService(session.NewExclusiveManager(), disk.storage, disk.SessionMetadata()),
	}, nil
}

func (c *Core) Close() error {
	if c == nil || c.disk == nil {
		return nil
	}
	return c.disk.Close()
}

func (c *Core) DiskID() string {
	return c.disk.DiskID()
}

func (c *Core) StoragePath() string {
	return c.disk.StoragePath()
}

func (c *Core) SessionService() *session.Service {
	return c.sessions
}

func (c *Core) RouteEntry(routeTarget string, connectionID uint64) route.Entry {
	return c.disk.RouteEntry(routeTarget, connectionID)
}

func (c *Core) GatewayRegisterInfo(gatewayToken string) gatewaylink.RegisterInfo {
	return c.disk.GatewayRegisterInfo(gatewayToken)
}
