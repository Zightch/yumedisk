use std::collections::{HashMap, HashSet};

use crate::resident::TwoQueueResident;

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub(super) enum ReadBlockAction {
    Copied,
    LoadRemote { block_index: u64, block_base: u64 },
    Rehydrate { block_index: u64 },
    Wait(WaitReason),
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub(super) enum WriteBlockAction {
    Patched,
    LoadRemote { block_index: u64, block_base: u64 },
    Rehydrate { block_index: u64 },
    Wait(WaitReason),
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub(super) enum WaitReason {
    Generic,
    DirtyEvictionTempSlot,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub(super) struct SpilledDirty {
    pub(super) valid_len: usize,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub(super) enum PrepareInsertResult {
    Reserved,
    Wait(WaitReason),
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub(super) enum WorkerAction {
    FlushTemp { block_index: u64, block_base: u64 },
    SnapshotCreated,
}

#[derive(Debug)]
pub(super) struct CacheState {
    pub(super) resident: TwoQueueResident,
    pub(super) spilled_dirty: HashMap<u64, SpilledDirty>,
    pub(super) active_temp_blocks: HashSet<u64>,
    pub(super) foreground_dirty_eviction_waiters: usize,
    pub(super) stop_requested: bool,
}

impl CacheState {
    pub(super) fn temp_file_count(&self) -> usize {
        self.spilled_dirty.len() + self.resident.active_snapshot_count()
    }

    pub(super) fn is_quiescent(&self) -> bool {
        if self.stop_requested
            || !self.spilled_dirty.is_empty()
            || !self.active_temp_blocks.is_empty()
            || self.foreground_dirty_eviction_waiters != 0
        {
            return false;
        }

        self.resident.is_quiescent()
    }
}
