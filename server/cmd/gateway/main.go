package main

import (
	"log"
	"os"

	"yumedisk/server/internal/config"
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

	log.Fatalf(
		"gateway runtime not implemented yet: client_addr=%s storer_addr=%s",
		cfg.Client.ListenAddr,
		cfg.Storer.ListenAddr,
	)
}
