package main

import (
	"context"
	"log"
	"os"
	"os/signal"
	"syscall"

	"yumedisk/server/internal/config"
	"yumedisk/server/internal/storer"
)

func main() {
	configPath, err := config.ExecutableConfigPath()
	if err != nil {
		log.Fatalf("resolve config path: %v", err)
	}
	cfg, initialized, err := config.LoadOrInitStorer(configPath, os.Stdin, os.Stdout)
	if err != nil {
		log.Fatalf("load config: %v", err)
	}
	if initialized {
		log.Printf("initialized storer config at %s", configPath)
	}

	diskIDRW, err := cfg.DiskIDRW()
	if err != nil {
		log.Fatalf("resolve disk_id_rw: %v", err)
	}
	diskIDRO := "ro_disabled"
	if value, ok, err := cfg.DiskIDRO(); err != nil {
		log.Fatalf("resolve disk_id_ro: %v", err)
	} else if ok {
		diskIDRO = value
	}

	runtime, err := storer.NewRoleRuntime(cfg)
	if err != nil {
		log.Fatalf("create runtime: %v", err)
	}
	defer func() {
		if err := runtime.Close(); err != nil {
			log.Printf("close runtime: %v", err)
		}
	}()

	switch runtime.Role() {
	case config.StorerRoleWhole:
		log.Printf(
			"whole runtime ready: addr=%s disk_id_rw=%s disk_id_ro=%s backend=%s",
			runtime.ListenAddr(),
			diskIDRW,
			diskIDRO,
			runtime.StoragePath(),
		)
	case config.StorerRoleStorer:
		log.Printf(
			"storer runtime ready: gateway_addr=%s disk_id_rw=%s disk_id_ro=%s backend=%s",
			runtime.GatewayAddr(),
			diskIDRW,
			diskIDRO,
			runtime.StoragePath(),
		)
	default:
		log.Fatalf("unsupported runtime role: %s", runtime.Role())
	}

	ctx, stop := signal.NotifyContext(context.Background(), os.Interrupt, syscall.SIGTERM)
	defer stop()

	if err := runtime.Run(ctx); err != nil {
		log.Fatalf("run runtime: %v", err)
	}
}
