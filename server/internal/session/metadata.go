package session

type Metadata struct {
	DiskID        string
	DiskSizeBytes uint64
	ReadOnly      bool
	MaxIOBytes    uint32
}

type Record struct {
	ID         uint64
	Connection uint64
	Metadata   Metadata
}
