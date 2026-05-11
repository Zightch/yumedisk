use core::ffi::c_char;
use core::ffi::c_void;

pub const YUMEDISK_MAX_TARGETS: u32 = 255;
pub const YUMEDISK_MIN_TARGET_ID: u32 = 0;
pub const YUMEDISK_MAX_USABLE_TARGET_ID: u32 = 254;

pub type AkStatus = i32;
pub const AK_STATUS_SUCCESS: AkStatus = 0x0000_0000;
pub const AK_STATUS_UNSUCCESSFUL: AkStatus = 0xC000_0001u32 as i32;
pub const AK_STATUS_NO_MORE_ENTRIES: AkStatus = 0x8000_001Au32 as i32;
pub const AK_STATUS_INVALID_PARAMETER: AkStatus = 0xC000_000Du32 as i32;

#[repr(C)]
#[allow(dead_code)]
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum AkLifecycleState {
    Init = 0,
    Starting = 1,
    Running = 2,
    Removing = 3,
    Closing = 4,
    Closed = 5,
    Broken = 6,
}

#[repr(C)]
#[allow(dead_code)]
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum AkEventType {
    DiskOnline = 0,
    DiskRemoved = 1,
    WriteFinalCommitted = 2,
    WriteFinalRejected = 3,
    SessionBroken = 4,
}

#[repr(C)]
pub struct AkSession {
    _private: [u8; 0],
}

#[repr(C)]
pub struct AkDisk {
    _private: [u8; 0],
}

pub type AkLogFn = unsafe extern "C" fn(log_ctx: *mut c_void, level: i32, text: *const c_char);

#[repr(C)]
#[derive(Clone, Copy)]
pub struct AkOpenParams {
    pub heartbeat_interval_ms: u32,
    pub initial_event_queue_capacity: u32,
    pub log_fn: Option<AkLogFn>,
    pub log_ctx: *mut c_void,
}

#[repr(C)]
#[derive(Clone, Copy)]
pub struct AkDiskParams {
    pub target_id: u32,
    pub sector_size: u32,
    pub disk_size_bytes: u64,
    pub queue_depth: u32,
    pub write_slot_bytes: u32,
    pub read_worker_count: u16,
    pub write_worker_count: u16,
    pub ack_batch_max_ranges: u32,
    pub read_only: u32,
}

#[repr(C)]
#[derive(Clone, Copy)]
pub struct AkReadOp {
    pub target_id: u32,
    pub disk_runtime_id: u64,
    pub event_id: u64,
    pub lba: u64,
    pub offset_bytes: u64,
    pub block_count: u32,
    pub data_length: u32,
    pub flags: u32,
}

#[repr(C)]
#[derive(Clone, Copy)]
pub struct AkWriteOp {
    pub target_id: u32,
    pub disk_runtime_id: u64,
    pub event_id: u64,
    pub seq: u32,
    pub total_seq: u32,
    pub lba: u64,
    pub offset_bytes: u64,
    pub byte_offset_in_write: u32,
    pub data_length: u32,
    pub flags: u32,
}

pub type AkReadBytesFn = unsafe extern "C" fn(
    media_ctx: *mut c_void,
    op: *const AkReadOp,
    out_buffer: *mut c_void,
    out_data_length: *mut u32,
) -> AkStatus;

pub type AkStageWriteFn = unsafe extern "C" fn(
    media_ctx: *mut c_void,
    op: *const AkWriteOp,
    data_buffer: *const c_void,
    data_length: u32,
) -> AkStatus;

#[repr(C)]
#[derive(Clone, Copy)]
pub struct AkMediaOps {
    pub read_bytes: Option<AkReadBytesFn>,
    pub stage_write: Option<AkStageWriteFn>,
}

#[repr(C)]
#[derive(Clone, Copy)]
pub struct AkEvent {
    pub event_type: AkEventType,
    pub target_id: u32,
    pub disk_runtime_id: u64,
    pub event_id: u64,
    pub total_seq: u32,
    pub flags: u32,
    pub status: AkStatus,
}

#[repr(C)]
#[derive(Clone, Copy)]
pub struct AkSessionState {
    pub lifecycle: AkLifecycleState,
    pub session_id: u64,
    pub heartbeat_running: u8,
    pub transport_ready: u8,
    pub disk_count: u32,
    pub appkernel_version_be: u32,
    pub kmdf_version_be: u32,
    pub scsi_version_be: u32,
    pub last_error: AkStatus,
}

#[repr(C)]
#[derive(Clone, Copy)]
pub struct AkDiskState {
    pub lifecycle: AkLifecycleState,
    pub target_id: u32,
    pub disk_runtime_id: u64,
    pub read_workers_running: u8,
    pub write_workers_running: u8,
    pub ack_flusher_running: u8,
    pub last_error: AkStatus,
}

#[repr(C)]
#[derive(Clone, Copy, Default)]
pub struct AkSessionStats {
    pub heartbeat_sent: u64,
    pub command_failures: u64,
    pub protocol_failures: u64,
    pub events_queued: u64,
    pub events_dropped: u64,
}

unsafe extern "C" {
    pub fn AkOpen(params: *const AkOpenParams, out_session: *mut *mut AkSession) -> AkStatus;
    pub fn AkClose(session: *mut AkSession);
    pub fn AkQuerySessionState(session: *mut AkSession, out_state: *mut AkSessionState)
    -> AkStatus;
    pub fn AkQuerySessionStats(session: *mut AkSession, out_stats: *mut AkSessionStats)
    -> AkStatus;
    pub fn AkPollEvent(session: *mut AkSession, out_event: *mut AkEvent) -> AkStatus;
    pub fn AkCreateDisk(
        session: *mut AkSession,
        params: *const AkDiskParams,
        media_ops: *const AkMediaOps,
        media_ctx: *mut c_void,
        out_disk: *mut *mut AkDisk,
    ) -> AkStatus;
    pub fn AkRemoveDisk(disk: *mut AkDisk) -> AkStatus;
    pub fn AkQueryDiskState(disk: *mut AkDisk, out_state: *mut AkDiskState) -> AkStatus;
}
