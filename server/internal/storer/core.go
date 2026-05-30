package storer

import (
	"crypto/rand"
	"fmt"
	"sync"

	"yumedisk/server/internal/config"
	"yumedisk/server/internal/session"
)

type DataChangedNotifier interface {
	NotifySessionDataChanged(sessionID uint64)
}

type Core struct {
	backendID [16]byte
	storage   *sharedStorage
	exports   map[ExportID]*Export

	dataChangedMu       sync.RWMutex
	dataChangedNotifier DataChangedNotifier
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

	var backendID [16]byte
	if _, err := rand.Read(backendID[:]); err != nil {
		_ = storage.Close()
		return nil, fmt.Errorf("generate backend_id: %w", err)
	}

	exports := map[ExportID]*Export{
		ExportIDRW: newRWExport(rwMaterial, storage, backendID),
	}

	if roMaterial, ok, err := cfg.ClaimMaterialRO(); err != nil {
		_ = storage.Close()
		return nil, fmt.Errorf("parse claim_code_ro: %w", err)
	} else if ok {
		exports[ExportIDRO] = newROExport(roMaterial, storage, backendID)
	}

	core := &Core{
		backendID: backendID,
		storage:   storage,
		exports:   exports,
	}
	if _, ok := exports[ExportIDRO]; ok {
		exports[ExportIDRW].SessionService().SetWriteCommittedHook(func(session.Record) {
			core.handleRWWriteCommitted()
		})
	}
	return core, nil
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

func (c *Core) BackendID() [16]byte {
	return c.backendID
}

func (c *Core) SetDataChangedNotifier(notifier DataChangedNotifier) {
	c.dataChangedMu.Lock()
	c.dataChangedNotifier = notifier
	c.dataChangedMu.Unlock()
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

func (c *Core) handleRWWriteCommitted() {
	roExport, ok := c.exports[ExportIDRO]
	if !ok {
		return
	}

	c.dataChangedMu.RLock()
	notifier := c.dataChangedNotifier
	c.dataChangedMu.RUnlock()
	if notifier == nil {
		return
	}

	for _, sessionID := range roExport.SessionService().LiveSessionIDs() {
		notifier.NotifySessionDataChanged(sessionID)
	}
}
