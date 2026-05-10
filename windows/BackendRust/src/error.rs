use thiserror::Error;

#[derive(Debug, Error, Clone, PartialEq, Eq)]
pub enum BackendError {
    #[error("invalid-heartbeat-interval-ms")]
    InvalidHeartbeatIntervalMs,
    #[error("invalid-initial-event-queue-capacity")]
    InvalidInitialEventQueueCapacity,
    #[error("invalid-target-id")]
    InvalidTargetId,
    #[error("invalid-sector-size")]
    InvalidSectorSize,
    #[error("invalid-disk-size-bytes")]
    InvalidDiskSizeBytes,
    #[error("invalid-queue-depth")]
    InvalidQueueDepth,
    #[error("invalid-write-slot-bytes")]
    InvalidWriteSlotBytes,
    #[error("invalid-read-worker-count")]
    InvalidReadWorkerCount,
    #[error("invalid-write-worker-count")]
    InvalidWriteWorkerCount,
    #[error("invalid-ack-batch-max-ranges")]
    InvalidAckBatchMaxRanges,
    #[error("invalid-media-instance")]
    InvalidMediaInstance,
    #[error("media-size-mismatch")]
    MediaSizeMismatch,
    #[error("session-not-open")]
    SessionNotOpen,
    #[error("target-already-exists")]
    TargetAlreadyExists,
    #[error("target-not-found")]
    TargetNotFound,
    #[error("no-free-target")]
    NoFreeTarget,
    #[error("invalid-parameter")]
    InvalidParameter,
}

impl BackendError {
    pub fn as_code(&self) -> &'static str {
        match self {
            Self::InvalidHeartbeatIntervalMs => "invalid-heartbeat-interval-ms",
            Self::InvalidInitialEventQueueCapacity => "invalid-initial-event-queue-capacity",
            Self::InvalidTargetId => "invalid-target-id",
            Self::InvalidSectorSize => "invalid-sector-size",
            Self::InvalidDiskSizeBytes => "invalid-disk-size-bytes",
            Self::InvalidQueueDepth => "invalid-queue-depth",
            Self::InvalidWriteSlotBytes => "invalid-write-slot-bytes",
            Self::InvalidReadWorkerCount => "invalid-read-worker-count",
            Self::InvalidWriteWorkerCount => "invalid-write-worker-count",
            Self::InvalidAckBatchMaxRanges => "invalid-ack-batch-max-ranges",
            Self::InvalidMediaInstance => "invalid-media-instance",
            Self::MediaSizeMismatch => "media-size-mismatch",
            Self::SessionNotOpen => "session-not-open",
            Self::TargetAlreadyExists => "target-already-exists",
            Self::TargetNotFound => "target-not-found",
            Self::NoFreeTarget => "no-free-target",
            Self::InvalidParameter => "invalid-parameter",
        }
    }
}
