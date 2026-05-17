package storer

import (
	"fmt"
	"time"

	"yumedisk/server/internal/auth"
	"yumedisk/server/internal/config"
	"yumedisk/server/internal/session"
	filestorage "yumedisk/server/internal/storage/file"
)

const (
	defaultSessionTTL   = 30 * time.Second
	defaultSessionMaxIO = 60 * 1024
)

type Core struct {
	cfg      config.StorerConfig
	material auth.Material
	storage  *filestorage.Backend
	sessions *session.Service
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

	return &Core{
		cfg:      cfg,
		material: material,
		storage:  storage,
		sessions: session.NewService(session.NewManager(), storage, defaultSessionTTL, defaultSessionMaxIO),
	}, nil
}

func (c *Core) Close() error {
	if c == nil || c.storage == nil {
		return nil
	}
	return c.storage.Close()
}

func (c *Core) DiskID() string {
	return c.material.DiskID
}

func (c *Core) AuthVerifier() [64]byte {
	return c.material.AuthVerifier
}

func (c *Core) StoragePath() string {
	return c.storage.Path()
}

func (c *Core) SessionService() *session.Service {
	return c.sessions
}
