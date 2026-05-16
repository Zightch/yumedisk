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
	cfg, initialized, err := config.LoadOrInit(configPath, os.Stdin, os.Stdout)
	if err != nil {
		log.Fatalf("load config: %v", err)
	}
	if initialized {
		log.Printf("initialized storer config at %s", configPath)
	}

	service, err := storer.NewService(cfg)
	if err != nil {
		log.Fatalf("create service: %v", err)
	}

	log.Printf(
		"embedded gateway storer ready: addr=%s disk_id=%s backend=%s",
		service.ListenAddr(),
		service.DiskID(),
		service.StoragePath(),
	)

	ctx, stop := signal.NotifyContext(context.Background(), os.Interrupt, syscall.SIGTERM)
	defer stop()

	if err := service.Run(ctx); err != nil {
		log.Fatalf("run service: %v", err)
	}
}
