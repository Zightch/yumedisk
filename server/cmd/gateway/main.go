package main

import (
	"context"
	"log"
	"os"
	"os/signal"
	"syscall"

	"yumedisk/server/internal/config"
	"yumedisk/server/internal/gateway"
)

func main() {
	configPath, err := config.ExecutableConfigPath()
	if err != nil {
		log.Fatalf("resolve config path: %v", err)
	}

	cfg, initialized, err := config.LoadOrInitGateway(configPath, os.Stdin, os.Stdout)
	if err != nil {
		log.Fatalf("load config: %v", err)
	}
	if initialized {
		log.Printf("initialized gateway config at %s", configPath)
	}

	runtime, err := gateway.NewRuntime(cfg)
	if err != nil {
		log.Fatalf("create gateway runtime: %v", err)
	}

	log.Printf(
		"gateway runtime ready: client_addr=%s storer_addr=%s",
		cfg.Client.ListenAddr,
		cfg.Storer.ListenAddr,
	)

	ctx, stop := signal.NotifyContext(context.Background(), os.Interrupt, syscall.SIGTERM)
	defer stop()

	if err := runtime.Run(ctx); err != nil {
		log.Fatalf("run gateway runtime: %v", err)
	}
}
