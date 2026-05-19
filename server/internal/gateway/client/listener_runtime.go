package client

import (
	"context"
	"errors"
	"fmt"
	"log"
	"net"
)

func ServeClientListener(
	ctx context.Context,
	listener net.Listener,
	logPrefix string,
	nextConnectionID func() uint64,
	serveAccepted func(context.Context, uint64, net.Conn),
) error {
	defer listener.Close()

	go func() {
		<-ctx.Done()
		_ = listener.Close()
	}()

	for {
		conn, err := listener.Accept()
		if err != nil {
			if ctx.Err() != nil {
				return nil
			}
			var netErr net.Error
			if errors.As(err, &netErr) && netErr.Temporary() {
				log.Printf("temporary %s accept error: %v", logPrefix, err)
				continue
			}
			return fmt.Errorf("accept %s connection: %w", logPrefix, err)
		}

		connectionID := nextConnectionID()
		go serveAccepted(ctx, connectionID, conn)
	}
}
