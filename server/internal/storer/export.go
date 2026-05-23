package storer

import (
	"yumedisk/server/internal/auth"
	"yumedisk/server/internal/route"
	"yumedisk/server/internal/session"
	gatewaylink "yumedisk/server/internal/storer/gateway/link"
)

const defaultSessionMaxIO = 60 * 1024

type ExportID string

const (
	ExportIDRW ExportID = "rw"
	ExportIDRO ExportID = "ro"
)

type Export struct {
	id       ExportID
	material auth.Material
	storage  *sharedStorage
	metadata session.Metadata
	sessions *session.Service
}

func newRWExport(material auth.Material, storage *sharedStorage) *Export {
	metadata := session.Metadata{
		DiskID:        material.DiskID,
		DiskSizeBytes: storage.Size(),
		ReadOnly:      false,
		MaxIOBytes:    defaultSessionMaxIO,
	}
	return &Export{
		id:       ExportIDRW,
		material: material,
		storage:  storage,
		metadata: metadata,
		sessions: session.NewService(session.NewExclusiveManager(), storage.backend, metadata),
	}
}

func newROExport(material auth.Material, storage *sharedStorage) *Export {
	metadata := session.Metadata{
		DiskID:        material.DiskID,
		DiskSizeBytes: storage.Size(),
		ReadOnly:      true,
		MaxIOBytes:    defaultSessionMaxIO,
	}
	return &Export{
		id:       ExportIDRO,
		material: material,
		storage:  storage,
		metadata: metadata,
		sessions: session.NewService(session.NewSharedManager(), storage.backend, metadata),
	}
}

func (e *Export) ID() ExportID {
	return e.id
}

func (e *Export) DiskID() string {
	return e.metadata.DiskID
}

func (e *Export) ReadOnly() bool {
	return e.metadata.ReadOnly
}

func (e *Export) Metadata() session.Metadata {
	return e.metadata
}

func (e *Export) SessionService() *session.Service {
	return e.sessions
}

func (e *Export) RouteEntry(routeTarget string, connectionID uint64) route.Entry {
	return route.Entry{
		DiskID:        e.metadata.DiskID,
		AuthVerifier:  e.material.AuthVerifier,
		RouteTarget:   routeTarget,
		ConnectionID:  connectionID,
		Connected:     true,
		DiskSizeBytes: e.metadata.DiskSizeBytes,
		ReadOnly:      e.metadata.ReadOnly,
		MaxIOBytes:    e.metadata.MaxIOBytes,
	}
}

func (e *Export) GatewayRegisterInfo(gatewayToken string) gatewaylink.RegisterInfo {
	return gatewaylink.RegisterInfo{
		GatewayToken:  gatewayToken,
		DiskID:        e.metadata.DiskID,
		AuthVerifier:  e.material.AuthVerifier,
		DiskSizeBytes: e.metadata.DiskSizeBytes,
		ReadOnly:      e.metadata.ReadOnly,
		MaxIOBytes:    e.metadata.MaxIOBytes,
	}
}
