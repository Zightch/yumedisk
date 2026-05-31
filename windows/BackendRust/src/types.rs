use crate::appkernel;

pub const SECTOR_ALIGNMENT_BYTES: u32 = 512;
pub const DEFAULT_SECTOR_SIZE: u32 = SECTOR_ALIGNMENT_BYTES;
pub const DEFAULT_QUEUE_DEPTH: u32 = 96;
pub const DEFAULT_WRITE_SLOT_BYTES: u32 = 1024 * 1024;
pub const DEFAULT_READ_WORKER_COUNT: u16 = 12;
pub const DEFAULT_WRITE_WORKER_COUNT: u16 = 12;
pub const DEFAULT_ACK_BATCH_MAX_RANGES: u32 = DEFAULT_QUEUE_DEPTH;
pub const EVENT_WAIT_POLL_MS: u32 = 100;
pub const DEFAULT_HEARTBEAT_INTERVAL_MS: u32 = 1000;
pub const DEFAULT_INITIAL_RESPONSE_QUEUE_CAPACITY: u32 = 1024;
pub const DEFAULT_INITIAL_SESSION_NOTICE_QUEUE_CAPACITY: u32 = 1024;
pub const MAX_BUFFERED_LOG_LINES: usize = 256;

pub const YUMEDISK_MIN_TARGET_ID: u32 = appkernel::YUMEDISK_MIN_TARGET_ID;
pub const YUMEDISK_MAX_USABLE_TARGET_ID: u32 = appkernel::YUMEDISK_MAX_USABLE_TARGET_ID;
pub const YUMEDISK_MAX_TARGETS: u32 = appkernel::YUMEDISK_MAX_TARGETS;

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum ManagedDiskResponseType {
    DiskOnline,
    DiskRemoved,
    WriteFinalCommitted,
    WriteFinalRejected,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct ManagedDiskResponse {
    pub response_type: ManagedDiskResponseType,
    pub target_id: u32,
    pub disk_runtime_id: u64,
    pub event_id: u64,
    pub total_seq: u32,
    pub flags: u32,
    pub status: i32,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum ManagedSessionNoticeType {
    Broken,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct ManagedSessionNotice {
    pub notice_type: ManagedSessionNoticeType,
    pub flags: u32,
    pub status: i32,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum ManagedDiskEventType {
    SystemEjected,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct ManagedDiskEvent {
    pub event_type: ManagedDiskEventType,
    pub target_id: u32,
    pub disk_runtime_id: u64,
    pub flags: u32,
    pub status: i32,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct SessionConfig {
    pub heartbeat_interval_ms: u32,
    pub initial_response_queue_capacity: u32,
    pub initial_session_notice_queue_capacity: u32,
}

impl Default for SessionConfig {
    fn default() -> Self {
        Self {
            heartbeat_interval_ms: DEFAULT_HEARTBEAT_INTERVAL_MS,
            initial_response_queue_capacity: DEFAULT_INITIAL_RESPONSE_QUEUE_CAPACITY,
            initial_session_notice_queue_capacity: DEFAULT_INITIAL_SESSION_NOTICE_QUEUE_CAPACITY,
        }
    }
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct DiskConfig {
    pub target_id: u32,
    pub sector_size: u32,
    pub disk_size_bytes: u64,
    pub queue_depth: u32,
    pub write_slot_bytes: u32,
    pub read_worker_count: u16,
    pub write_worker_count: u16,
    pub ack_batch_max_ranges: u32,
    pub read_only: bool,
}

impl Default for DiskConfig {
    fn default() -> Self {
        Self {
            target_id: YUMEDISK_MAX_TARGETS,
            sector_size: DEFAULT_SECTOR_SIZE,
            disk_size_bytes: 0,
            queue_depth: DEFAULT_QUEUE_DEPTH,
            write_slot_bytes: DEFAULT_WRITE_SLOT_BYTES,
            read_worker_count: DEFAULT_READ_WORKER_COUNT,
            write_worker_count: DEFAULT_WRITE_WORKER_COUNT,
            ack_batch_max_ranges: DEFAULT_ACK_BATCH_MAX_RANGES,
            read_only: false,
        }
    }
}

#[derive(Debug, Clone, Default, PartialEq, Eq)]
pub struct ManagedDiskSnapshot {
    pub target_id: u32,
    pub disk_size_bytes: u64,
    pub sector_size: u32,
    pub read_only: bool,
    pub lifecycle_text: String,
    pub online: bool,
}

#[derive(Debug, Clone, Default, PartialEq, Eq)]
pub struct BackendStatsSnapshot {
    pub heartbeat_sent: u64,
    pub command_failures: u64,
    pub protocol_failures: u64,
    pub responses_queued: u64,
    pub responses_dropped: u64,
    pub session_notices_queued: u64,
    pub session_notices_dropped: u64,
    pub disk_count: u64,
}

#[derive(Debug, Clone, Default, PartialEq, Eq)]
pub struct DebugSnapshot {
    pub session_state_text: String,
    pub stats: BackendStatsSnapshot,
    pub disks: Vec<ManagedDiskSnapshot>,
}

#[derive(Debug, Clone, Default, PartialEq, Eq)]
pub struct ComponentVersionSnapshot {
    pub appkernel_version_text: String,
    pub kmdf_version_text: String,
    pub scsi_version_text: String,
}

#[derive(Debug, Clone, Default, PartialEq, Eq)]
pub struct DiskMetadata {
    pub target_id: u32,
    pub sector_size: u32,
    pub disk_size_bytes: u64,
    pub read_only: bool,
}

#[derive(Debug, Clone, Default, PartialEq, Eq)]
pub struct DiskQueueConfig {
    pub queue_depth: u32,
    pub write_slot_bytes: u32,
    pub read_worker_count: u16,
    pub write_worker_count: u16,
    pub ack_batch_max_ranges: u32,
}
