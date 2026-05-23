package storer

import (
	"context"
	"fmt"
	"log"
	"sync"
	"sync/atomic"
	"time"

	"yumedisk/server/internal/config"
	storegatewaylink "yumedisk/server/internal/storer/gateway/link"
)

type Runtime interface {
	Run(ctx context.Context) error
}

const storerLinkHeartbeatTimeout = 15 * time.Second
const storerLinkReconnectInterval = 5 * time.Second

type StorerRuntime struct {
	links    []*storerLinkWorker
	nextConn atomic.Uint64
}

func NewStorerRuntime(cfg config.StorerConfig, core *Core) (*StorerRuntime, error) {
	if cfg.Role != config.StorerRoleStorer {
		return nil, fmt.Errorf("storer runtime requires role=%q", config.StorerRoleStorer)
	}
	if core == nil {
		return nil, fmt.Errorf("storer runtime requires non-nil core")
	}

	rwExport, ok := core.Export(ExportIDRW)
	if !ok {
		return nil, fmt.Errorf("storer runtime requires rw export")
	}

	rwLinkRuntime, err := storegatewaylink.NewLinkRuntime(
		cfg.Storer.GatewayAddr,
		rwExport.GatewayRegisterInfo(cfg.Storer.GatewayToken),
		rwExport.SessionService(),
		storerLinkHeartbeatTimeout,
	)
	if err != nil {
		return nil, err
	}

	links := []*storerLinkWorker{
		newStorerLinkWorker("rw", rwLinkRuntime),
	}

	if roExport, ok := core.Export(ExportIDRO); ok {
		roLinkRuntime, err := storegatewaylink.NewLinkRuntime(
			cfg.Storer.GatewayAddr,
			roExport.GatewayRegisterInfo(cfg.Storer.GatewayToken),
			roExport.SessionService(),
			storerLinkHeartbeatTimeout,
		)
		if err != nil {
			return nil, err
		}
		links = append(links, newStorerLinkWorker("ro", roLinkRuntime))
	}

	return &StorerRuntime{
		links: links,
	}, nil
}

func (r *StorerRuntime) Run(ctx context.Context) error {
	var wg sync.WaitGroup
	for _, link := range r.links {
		wg.Add(1)
		go func(link *storerLinkWorker) {
			defer wg.Done()
			link.Run(ctx, &r.nextConn)
		}(link)
	}

	<-ctx.Done()
	wg.Wait()
	return nil
}

type RoleRuntime struct {
	cfg     config.StorerConfig
	core    *Core
	runtime Runtime
}

func NewRoleRuntime(cfg config.StorerConfig) (*RoleRuntime, error) {
	core, err := NewCore(cfg)
	if err != nil {
		return nil, err
	}

	runtime, err := newRuntimeForRole(cfg, core)
	if err != nil {
		_ = core.Close()
		return nil, err
	}

	return &RoleRuntime{
		cfg:     cfg,
		core:    core,
		runtime: runtime,
	}, nil
}

func (r *RoleRuntime) Close() error {
	if r == nil || r.core == nil {
		return nil
	}
	return r.core.Close()
}

func (r *RoleRuntime) Run(ctx context.Context) error {
	return r.runtime.Run(ctx)
}

func (r *RoleRuntime) Role() config.StorerRole {
	return r.cfg.Role
}

func (r *RoleRuntime) StoragePath() string {
	return r.core.StoragePath()
}

func (r *RoleRuntime) ListenAddr() string {
	if runtime, ok := r.runtime.(*WholeRuntime); ok {
		return runtime.ListenAddr()
	}
	return ""
}

func (r *RoleRuntime) GatewayAddr() string {
	if r.cfg.Role != config.StorerRoleStorer {
		return ""
	}
	return r.cfg.Storer.GatewayAddr
}

func newRuntimeForRole(cfg config.StorerConfig, core *Core) (Runtime, error) {
	switch cfg.Role {
	case config.StorerRoleWhole:
		return NewWholeRuntime(cfg, core)
	case config.StorerRoleStorer:
		return NewStorerRuntime(cfg, core)
	default:
		return nil, fmt.Errorf("unsupported role: %q", cfg.Role)
	}
}

type storerLinkWorker struct {
	label           string
	linkRuntime     *storegatewaylink.LinkRuntime
	retryInterval   time.Duration
	logf            func(format string, args ...any)
	reconnectingLog bool
}

func newStorerLinkWorker(label string, linkRuntime *storegatewaylink.LinkRuntime) *storerLinkWorker {
	return &storerLinkWorker{
		label:         label,
		linkRuntime:   linkRuntime,
		retryInterval: storerLinkReconnectInterval,
		logf:          log.Printf,
	}
}

func (w *storerLinkWorker) Run(ctx context.Context, nextConn *atomic.Uint64) {
	if w == nil || w.linkRuntime == nil {
		return
	}

	for {
		if ctx.Err() != nil {
			return
		}

		connectionID := nextConn.Add(1)
		_ = w.linkRuntime.Run(ctx, connectionID, func() {
			if w.reconnectingLog {
				w.logf("%s重连成功", w.label)
				w.reconnectingLog = false
			}
		})
		if ctx.Err() != nil {
			return
		}

		if !w.reconnectingLog {
			w.logf("%s重连中...", w.label)
			w.reconnectingLog = true
		}

		timer := time.NewTimer(w.retryInterval)
		select {
		case <-ctx.Done():
			if !timer.Stop() {
				<-timer.C
			}
			return
		case <-timer.C:
		}
	}
}
