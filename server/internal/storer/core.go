package storer

import (
	"fmt"

	"yumedisk/server/internal/auth"
	"yumedisk/server/internal/config"
	"yumedisk/server/internal/route"
	"yumedisk/server/internal/session"
	filestorage "yumedisk/server/internal/storage/file"
	gatewaylink "yumedisk/server/internal/storer/gateway/link"
)

const (
	defaultSessionMaxIO = 60 * 1024
)

type Core struct {
	material auth.Material
	storage  *filestorage.Backend
	sessions *session.Service
	metadata session.Metadata
}

func NewCore(cfg config.StorerConfig) (*Core, error) {
	material, err := auth.ParseClaimCode(cfg.ClaimCode)
	if err != nil {
		return nil, fmt.Errorf("parse claim code: %w", err)
	}

	storage, err := filestorage.Open(cfg.StorageFilePath, false)
	if err != nil {
		return nil, err
	}

	metadata := session.Metadata{
		DiskID:        material.DiskID,
		DiskSizeBytes: storage.Size(),
		ReadOnly:      storage.ReadOnly(),
		MaxIOBytes:    defaultSessionMaxIO,
	}

	return &Core{
		material: material,
		storage:  storage,
		metadata: metadata,
		sessions: session.NewService(session.NewManager(), storage, metadata),
	}, nil
}

func (c *Core) Close() error {
	if c == nil || c.storage == nil {
		return nil
	}
	return c.storage.Close()
}

func (c *Core) DiskID() string {
	return c.metadata.DiskID
}

func (c *Core) StoragePath() string {
	return c.storage.Path()
}

func (c *Core) SessionService() *session.Service {
	return c.sessions
}

func (c *Core) RouteEntry(routeTarget string, connectionID uint64) route.Entry {
	return route.Entry{
		DiskID:        c.metadata.DiskID,
		AuthVerifier:  c.material.AuthVerifier,
		RouteTarget:   routeTarget,
		ConnectionID:  connectionID,
		Connected:     true,
		DiskSizeBytes: c.metadata.DiskSizeBytes,
		ReadOnly:      c.metadata.ReadOnly,
		MaxIOBytes:    c.metadata.MaxIOBytes,
	}
}

func (c *Core) GatewayRegisterInfo(gatewayToken string) gatewaylink.RegisterInfo {
	return gatewaylink.RegisterInfo{
		GatewayToken:  gatewayToken,
		DiskID:        c.metadata.DiskID,
		AuthVerifier:  c.material.AuthVerifier,
		DiskSizeBytes: c.metadata.DiskSizeBytes,
		ReadOnly:      c.metadata.ReadOnly,
		MaxIOBytes:    c.metadata.MaxIOBytes,
	}
}
