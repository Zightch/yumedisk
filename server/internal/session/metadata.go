package session

// MaxDataPlaneRawBytes limits the business-level raw data bytes carried by a
// single ReadAt or WriteAt message: before encoding/compression on send, and
// after decoding/decompression on receive. It is independent from transport
// frame sizing.
const MaxDataPlaneRawBytes uint32 = 60 * 1024

type Metadata struct {
	DiskID        string
	DiskSizeBytes uint64
	ReadOnly      bool
	// Compatibility field for on-wire session metadata. Production data-plane
	// semantics normalize this to MaxDataPlaneRawBytes.
	MaxIOBytes uint32
	BackendID  [16]byte
}

type Record struct {
	ID         uint64
	Connection uint64
	Closing    bool
	Metadata   Metadata
}
