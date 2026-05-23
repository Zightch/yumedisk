package storer

import (
	"fmt"

	"yumedisk/server/internal/config"
)

type Core struct {
	storage *sharedStorage
	exports map[ExportID]*Export
}

func NewCore(cfg config.StorerConfig) (*Core, error) {
	storage, err := openSharedStorage(cfg.StorageFilePath)
	if err != nil {
		return nil, err
	}

	rwMaterial, err := cfg.ClaimMaterialRW()
	if err != nil {
		_ = storage.Close()
		return nil, fmt.Errorf("parse claim_code_rw: %w", err)
	}

	exports := map[ExportID]*Export{
		ExportIDRW: newRWExport(rwMaterial, storage),
	}

	if roMaterial, ok, err := cfg.ClaimMaterialRO(); err != nil {
		_ = storage.Close()
		return nil, fmt.Errorf("parse claim_code_ro: %w", err)
	} else if ok {
		exports[ExportIDRO] = newROExport(roMaterial, storage)
	}

	return &Core{
		storage: storage,
		exports: exports,
	}, nil
}

func (c *Core) Close() error {
	if c == nil || c.storage == nil {
		return nil
	}
	return c.storage.Close()
}

func (c *Core) StoragePath() string {
	return c.storage.Path()
}

func (c *Core) ExportIDs() []ExportID {
	ids := []ExportID{ExportIDRW}
	if _, ok := c.exports[ExportIDRO]; ok {
		ids = append(ids, ExportIDRO)
	}
	return ids
}

func (c *Core) Export(id ExportID) (*Export, bool) {
	export, ok := c.exports[id]
	return export, ok
}
