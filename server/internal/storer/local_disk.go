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

const defaultSessionMaxIO = 60 * 1024

type localDisk struct {
	material auth.Material
	storage  *filestorage.Backend
	metadata session.Metadata
}

func openLocalDisk(cfg config.StorerConfig) (*localDisk, error) {
	material, err := auth.ParseClaimCode(cfg.ClaimCode)
	if err != nil {
		return nil, fmt.Errorf("parse claim code: %w", err)
	}

	storage, err := filestorage.Open(cfg.StorageFilePath, false)
	if err != nil {
		return nil, err
	}

	return &localDisk{
		material: material,
		storage:  storage,
		metadata: session.Metadata{
			DiskID:        material.DiskID,
			DiskSizeBytes: storage.Size(),
			ReadOnly:      storage.ReadOnly(),
			MaxIOBytes:    defaultSessionMaxIO,
		},
	}, nil
}

func (d *localDisk) Close() error {
	if d == nil || d.storage == nil {
		return nil
	}
	return d.storage.Close()
}

func (d *localDisk) DiskID() string {
	return d.metadata.DiskID
}

func (d *localDisk) StoragePath() string {
	return d.storage.Path()
}

func (d *localDisk) SessionMetadata() session.Metadata {
	return d.metadata
}

func (d *localDisk) RouteEntry(routeTarget string, connectionID uint64) route.Entry {
	return route.Entry{
		DiskID:        d.metadata.DiskID,
		AuthVerifier:  d.material.AuthVerifier,
		RouteTarget:   routeTarget,
		ConnectionID:  connectionID,
		Connected:     true,
		DiskSizeBytes: d.metadata.DiskSizeBytes,
		ReadOnly:      d.metadata.ReadOnly,
		MaxIOBytes:    d.metadata.MaxIOBytes,
	}
}

func (d *localDisk) GatewayRegisterInfo(gatewayToken string) gatewaylink.RegisterInfo {
	return gatewaylink.RegisterInfo{
		GatewayToken:  gatewayToken,
		DiskID:        d.metadata.DiskID,
		AuthVerifier:  d.material.AuthVerifier,
		DiskSizeBytes: d.metadata.DiskSizeBytes,
		ReadOnly:      d.metadata.ReadOnly,
		MaxIOBytes:    d.metadata.MaxIOBytes,
	}
}
