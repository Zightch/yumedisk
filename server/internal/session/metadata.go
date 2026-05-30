package session

type Metadata struct {
	DiskID        string
	DiskSizeBytes uint64
	ReadOnly      bool
	MaxIOBytes    uint32
	BackendID     [16]byte
}

type Record struct {
	ID         uint64
	Connection uint64
	Closing    bool
	Metadata   Metadata
}
