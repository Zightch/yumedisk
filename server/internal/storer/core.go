package storer

import (
	"fmt"

	"yumedisk/server/internal/auth"
	"yumedisk/server/internal/config"
	"yumedisk/server/internal/route"
	"yumedisk/server/internal/session"
	filestorage "yumedisk/server/internal/storage/file"
)

const (
	defaultSessionMaxIO = 60 * 1024
)

type Core struct {
	cfg      config.StorerConfig
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
		cfg:      cfg,
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

func (c *Core) AuthVerifier() [64]byte {
	return c.material.AuthVerifier
}

func (c *Core) DiskSize() uint64 {
	return c.metadata.DiskSizeBytes
}

func (c *Core) ReadOnly() bool {
	return c.metadata.ReadOnly
}

func (c *Core) StoragePath() string {
	return c.storage.Path()
}

func (c *Core) SessionService() *session.Service {
	return c.sessions
}

func (c *Core) SessionMetadata() session.Metadata {
	return c.metadata
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
