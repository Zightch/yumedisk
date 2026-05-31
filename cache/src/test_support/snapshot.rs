use std::ops::Range;

use crate::resident::{LoadState, QueueRef};

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum QueueKindSnapshot {
    Fifo,
    Lru,
}

impl From<QueueRef> for QueueKindSnapshot {
    fn from(value: QueueRef) -> Self {
        match value {
            QueueRef::Fifo(_) => Self::Fifo,
            QueueRef::Lru(_) => Self::Lru,
        }
    }
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum LoadStateSnapshot {
    LoadingRemote,
    Rehydrating,
    Ready,
}

impl From<LoadState> for LoadStateSnapshot {
    fn from(value: LoadState) -> Self {
        match value {
            LoadState::LoadingRemote => Self::LoadingRemote,
            LoadState::Rehydrating => Self::Rehydrating,
            LoadState::Ready => Self::Ready,
        }
    }
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct ResidentBlockSnapshot {
    pub block_index: u64,
    pub queue: QueueKindSnapshot,
    pub load_state: LoadStateSnapshot,
    pub dirty_ranges: Vec<Range<usize>>,
    pub has_active_snapshot: bool,
    pub pending_patch_count: usize,
    pub valid_len: usize,
}

#[derive(Debug, Clone, PartialEq, Eq, Default)]
pub struct ResidentSnapshot {
    pub fifo_order: Vec<u64>,
    pub lru_order: Vec<u64>,
    pub blocks: Vec<ResidentBlockSnapshot>,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct SpilledDirtySnapshot {
    pub block_index: u64,
    pub valid_len: usize,
}

#[derive(Debug, Clone, PartialEq, Eq, Default)]
pub struct CacheSnapshot {
    pub resident: ResidentSnapshot,
    pub spilled_dirty: Vec<SpilledDirtySnapshot>,
    pub active_temp_blocks: Vec<u64>,
    pub foreground_dirty_eviction_waiters: usize,
    pub stop_requested: bool,
}
