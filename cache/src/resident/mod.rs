#![allow(dead_code)]

mod list;

use std::collections::HashMap;
use std::ops::Range;

use list::{IndexedList, ListNodeId};

use crate::CacheError;

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub(crate) enum QueueRef {
    Fifo(ListNodeId),
    Lru(ListNodeId),
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub(crate) enum LoadState {
    LoadingRemote,
    Rehydrating,
    Ready,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub(crate) struct ActiveSnapshot;

#[derive(Debug, Clone, PartialEq, Eq)]
pub(crate) struct PendingPatch {
    pub(crate) range: Range<usize>,
    pub(crate) data: Vec<u8>,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub(crate) struct BlockState {
    pub(crate) queue_ref: QueueRef,
    pub(crate) load_state: LoadState,
    pub(crate) dirty_ranges: Vec<Range<usize>>,
    pub(crate) active_snapshot: Option<ActiveSnapshot>,
    pub(crate) pending_patches: Vec<PendingPatch>,
    pub(crate) valid_len: usize,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub(crate) struct BlockEntry {
    pub(crate) data: Box<[u8]>,
    pub(crate) state: BlockState,
}

impl BlockEntry {
    fn new_loading(
        block_size: usize,
        queue_ref: QueueRef,
        load_state: LoadState,
        valid_len: usize,
        active_snapshot: Option<ActiveSnapshot>,
    ) -> Self {
        Self {
            data: vec![0; block_size].into_boxed_slice(),
            state: BlockState {
                queue_ref,
                load_state,
                dirty_ranges: Vec::new(),
                active_snapshot,
                pending_patches: Vec::new(),
                valid_len,
            },
        }
    }

    fn new_ready(data: Vec<u8>, queue_ref: QueueRef, valid_len: usize) -> Self {
        Self {
            data: data.into_boxed_slice(),
            state: BlockState {
                queue_ref,
                load_state: LoadState::Ready,
                dirty_ranges: Vec::new(),
                active_snapshot: None,
                pending_patches: Vec::new(),
                valid_len,
            },
        }
    }
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub(crate) struct EvictedBlock {
    pub(crate) block_index: u64,
    pub(crate) entry: BlockEntry,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub(crate) struct InsertResult {
    pub(crate) evicted: Option<EvictedBlock>,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub(crate) enum BeginLoadResult {
    Started,
    Wait,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub(crate) struct TouchResult {
    pub(crate) promoted: bool,
    pub(crate) evicted: Option<EvictedBlock>,
}

#[derive(Debug)]
pub(crate) struct TwoQueueResident {
    block_size: usize,
    fifo_capacity: usize,
    lru_capacity: usize,
    blocks: HashMap<u64, BlockEntry>,
    fifo: IndexedList,
    lru: IndexedList,
}

impl TwoQueueResident {
    pub(crate) fn new(
        fifo_capacity: usize,
        lru_capacity: usize,
        block_size: usize,
    ) -> Result<Self, CacheError> {
        if fifo_capacity == 0 {
            return Err(CacheError::InvalidConfig(
                "fifo_capacity_blocks must be greater than 0",
            ));
        }

        if lru_capacity == 0 {
            return Err(CacheError::InvalidConfig(
                "lru_capacity_blocks must be greater than 0",
            ));
        }

        if block_size == 0 {
            return Err(CacheError::InvalidConfig(
                "block_size_bytes must be greater than 0",
            ));
        }

        Ok(Self {
            block_size,
            fifo_capacity,
            lru_capacity,
            blocks: HashMap::new(),
            fifo: IndexedList::default(),
            lru: IndexedList::default(),
        })
    }

    pub(crate) fn len(&self) -> usize {
        self.blocks.len()
    }

    pub(crate) fn get(&self, block_index: u64) -> Option<&BlockEntry> {
        self.blocks.get(&block_index)
    }

    pub(crate) fn get_mut(&mut self, block_index: u64) -> Option<&mut BlockEntry> {
        self.blocks.get_mut(&block_index)
    }

    pub(crate) fn insert_ready(
        &mut self,
        block_index: u64,
        data: Vec<u8>,
        valid_len: usize,
    ) -> Result<InsertResult, CacheError> {
        self.validate_insert(block_index, data.len(), valid_len)?;

        let evicted = if self.fifo.len() == self.fifo_capacity {
            self.evict_fifo_head()?
        } else {
            None
        };

        let node = self.fifo.push_back(block_index);
        let entry = BlockEntry::new_ready(data, QueueRef::Fifo(node), valid_len);
        let replaced = self.blocks.insert(block_index, entry);
        debug_assert!(replaced.is_none());
        Ok(InsertResult { evicted })
    }

    pub(crate) fn begin_remote_load(
        &mut self,
        block_index: u64,
        valid_len: usize,
    ) -> Result<BeginLoadResult, CacheError> {
        self.validate_loading_insert(block_index, valid_len)?;

        if self.fifo.len() == self.fifo_capacity {
            if !self.can_evict_fifo_head_for_insert()? {
                return Ok(BeginLoadResult::Wait);
            }

            let _ = self.evict_fifo_head()?;
        }

        self.insert_loading_placeholder(block_index, valid_len, LoadState::LoadingRemote, None)?;
        Ok(BeginLoadResult::Started)
    }

    pub(crate) fn insert_loading_placeholder(
        &mut self,
        block_index: u64,
        valid_len: usize,
        load_state: LoadState,
        active_snapshot: Option<ActiveSnapshot>,
    ) -> Result<(), CacheError> {
        self.validate_loading_insert(block_index, valid_len)?;
        if self.fifo.len() == self.fifo_capacity {
            return Err(CacheError::InvariantViolation(
                "fifo must have free capacity before inserting loading placeholder",
            ));
        }

        let node = self.fifo.push_back(block_index);
        let entry = BlockEntry::new_loading(
            self.block_size,
            QueueRef::Fifo(node),
            load_state,
            valid_len,
            active_snapshot,
        );
        let replaced = self.blocks.insert(block_index, entry);
        debug_assert!(replaced.is_none());
        Ok(())
    }

    pub(crate) fn finish_remote_load(
        &mut self,
        block_index: u64,
        data: Vec<u8>,
    ) -> Result<(), CacheError> {
        if data.len() != self.block_size {
            return Err(CacheError::InvalidBlockDataLength {
                expected: self.block_size,
                actual: data.len(),
            });
        }

        let entry = self
            .blocks
            .get_mut(&block_index)
            .ok_or(CacheError::InvariantViolation(
                "loading resident block missing during load completion",
            ))?;
        if !matches!(
            entry.state.load_state,
            LoadState::LoadingRemote | LoadState::Rehydrating
        ) {
            return Err(CacheError::InvariantViolation(
                "resident block load completion requires loading state",
            ));
        }

        entry.data = data.into_boxed_slice();
        Self::apply_pending_patches(entry)?;
        entry.state.load_state = LoadState::Ready;
        Ok(())
    }

    pub(crate) fn enqueue_pending_patch(
        &mut self,
        block_index: u64,
        range: Range<usize>,
        data: &[u8],
    ) -> Result<(), CacheError> {
        let entry = self
            .blocks
            .get_mut(&block_index)
            .ok_or(CacheError::InvariantViolation(
                "loading resident block missing during pending patch enqueue",
            ))?;
        if !matches!(
            entry.state.load_state,
            LoadState::LoadingRemote | LoadState::Rehydrating
        ) {
            return Err(CacheError::InvariantViolation(
                "pending patch enqueue requires loading state",
            ));
        }

        Self::validate_patch(&range, data.len(), entry.data.len())?;
        entry.state.pending_patches.push(PendingPatch {
            range,
            data: data.to_vec(),
        });
        Ok(())
    }

    pub(crate) fn patch_ready_block(
        &mut self,
        block_index: u64,
        range: Range<usize>,
        data: &[u8],
    ) -> Result<(), CacheError> {
        let entry = self
            .blocks
            .get_mut(&block_index)
            .ok_or(CacheError::InvariantViolation(
                "ready resident block missing during patch",
            ))?;
        if entry.state.load_state != LoadState::Ready {
            return Err(CacheError::InvariantViolation(
                "resident block patch requires ready state",
            ));
        }

        Self::apply_patch(entry, range, data)
    }

    pub(crate) fn abort_remote_load(&mut self, block_index: u64) -> Result<(), CacheError> {
        let load_state = self
            .blocks
            .get(&block_index)
            .map(|entry| entry.state.load_state)
            .ok_or(CacheError::InvariantViolation(
                "loading resident block missing during load rollback",
            ))?;
        if !matches!(
            load_state,
            LoadState::LoadingRemote | LoadState::Rehydrating
        ) {
            return Err(CacheError::InvariantViolation(
                "resident block load rollback requires loading state",
            ));
        }

        let _ = self
            .evict_block(block_index)?
            .ok_or(CacheError::InvariantViolation(
                "loading resident block missing during queue rollback",
            ))?;
        Ok(())
    }

    pub(crate) fn mark_snapshot_written(&mut self, block_index: u64) -> Result<(), CacheError> {
        let entry = self
            .blocks
            .get_mut(&block_index)
            .ok_or(CacheError::InvariantViolation(
                "resident block missing during snapshot activation",
            ))?;
        if entry.state.load_state != LoadState::Ready {
            return Err(CacheError::InvariantViolation(
                "snapshot activation requires ready resident block",
            ));
        }

        entry.state.active_snapshot = Some(ActiveSnapshot);
        entry.state.dirty_ranges.clear();
        Ok(())
    }

    pub(crate) fn attach_active_snapshot(&mut self, block_index: u64) -> Result<(), CacheError> {
        let entry = self
            .blocks
            .get_mut(&block_index)
            .ok_or(CacheError::InvariantViolation(
                "resident block missing during active snapshot attach",
            ))?;
        if entry.state.load_state != LoadState::Ready {
            return Err(CacheError::InvariantViolation(
                "active snapshot attach requires ready resident block",
            ));
        }

        entry.state.active_snapshot = Some(ActiveSnapshot);
        Ok(())
    }

    pub(crate) fn record_hit(
        &mut self,
        block_index: u64,
    ) -> Result<Option<TouchResult>, CacheError> {
        let (queue_ref, load_state) = match self.blocks.get(&block_index) {
            Some(entry) => (entry.state.queue_ref, entry.state.load_state),
            None => return Ok(None),
        };
        if load_state != LoadState::Ready {
            return Err(CacheError::InvariantViolation(
                "resident hit recorded before block became ready",
            ));
        }

        match queue_ref {
            QueueRef::Fifo(node) => {
                if self.lru.len() == self.lru_capacity
                    && !self.can_evict_lru_head_for_promotion()?
                {
                    return Ok(Some(TouchResult {
                        promoted: false,
                        evicted: None,
                    }));
                }

                let removed = self
                    .fifo
                    .remove(node)
                    .ok_or(CacheError::InvariantViolation(
                        "fifo node missing during promotion",
                    ))?;
                if removed != block_index {
                    return Err(CacheError::InvariantViolation(
                        "fifo node value mismatch during promotion",
                    ));
                }

                let evicted = if self.lru.len() == self.lru_capacity {
                    self.evict_lru_head()?
                } else {
                    None
                };

                let new_node = self.lru.push_back(block_index);
                self.blocks.get_mut(&block_index).unwrap().state.queue_ref =
                    QueueRef::Lru(new_node);

                Ok(Some(TouchResult {
                    promoted: true,
                    evicted,
                }))
            }
            QueueRef::Lru(node) => {
                if !self.lru.move_to_back(node) {
                    return Err(CacheError::InvariantViolation(
                        "lru node missing during hit refresh",
                    ));
                }

                Ok(Some(TouchResult {
                    promoted: false,
                    evicted: None,
                }))
            }
        }
    }

    pub(crate) fn select_insert_victim(&self) -> Option<u64> {
        if self.fifo.len() < self.fifo_capacity {
            return None;
        }

        self.fifo.front_value()
    }

    pub(crate) fn select_promotion_victim(&self) -> Option<u64> {
        if self.lru.len() < self.lru_capacity {
            return None;
        }

        self.lru.front_value()
    }

    pub(crate) fn active_snapshot_count(&self) -> usize {
        self.blocks
            .values()
            .filter(|entry| entry.state.active_snapshot.is_some())
            .count()
    }

    pub(crate) fn evict_block(
        &mut self,
        block_index: u64,
    ) -> Result<Option<EvictedBlock>, CacheError> {
        let queue_ref = match self.blocks.get(&block_index) {
            Some(entry) => entry.state.queue_ref,
            None => return Ok(None),
        };

        match queue_ref {
            QueueRef::Fifo(node) => {
                let removed = self
                    .fifo
                    .remove(node)
                    .ok_or(CacheError::InvariantViolation(
                        "fifo node missing during block eviction",
                    ))?;
                if removed != block_index {
                    return Err(CacheError::InvariantViolation(
                        "fifo node value mismatch during block eviction",
                    ));
                }
            }
            QueueRef::Lru(node) => {
                let removed = self.lru.remove(node).ok_or(CacheError::InvariantViolation(
                    "lru node missing during block eviction",
                ))?;
                if removed != block_index {
                    return Err(CacheError::InvariantViolation(
                        "lru node value mismatch during block eviction",
                    ));
                }
            }
        }

        let entry = self
            .blocks
            .remove(&block_index)
            .ok_or(CacheError::InvariantViolation(
                "resident block missing after queue removal",
            ))?;
        Ok(Some(EvictedBlock { block_index, entry }))
    }

    #[cfg(test)]
    fn fifo_values(&self) -> Vec<u64> {
        self.fifo.values_from_head()
    }

    #[cfg(test)]
    fn lru_values(&self) -> Vec<u64> {
        self.lru.values_from_head()
    }

    fn validate_insert(
        &self,
        block_index: u64,
        data_len: usize,
        valid_len: usize,
    ) -> Result<(), CacheError> {
        if self.blocks.contains_key(&block_index) {
            return Err(CacheError::ResidentBlockAlreadyExists { block_index });
        }

        if data_len != self.block_size {
            return Err(CacheError::InvalidBlockDataLength {
                expected: self.block_size,
                actual: data_len,
            });
        }

        self.validate_valid_len(valid_len)
    }

    fn validate_loading_insert(
        &self,
        block_index: u64,
        valid_len: usize,
    ) -> Result<(), CacheError> {
        if self.blocks.contains_key(&block_index) {
            return Err(CacheError::ResidentBlockAlreadyExists { block_index });
        }

        self.validate_valid_len(valid_len)
    }

    fn validate_valid_len(&self, valid_len: usize) -> Result<(), CacheError> {
        if valid_len == 0 || valid_len > self.block_size {
            return Err(CacheError::InvalidValidLength {
                valid_len,
                block_size: self.block_size,
            });
        }

        Ok(())
    }

    fn can_evict_fifo_head_for_insert(&self) -> Result<bool, CacheError> {
        let block_index = self
            .fifo
            .front_value()
            .ok_or(CacheError::InvariantViolation(
                "fifo missing head while insert path reports full",
            ))?;
        let entry = self
            .blocks
            .get(&block_index)
            .ok_or(CacheError::InvariantViolation(
                "resident block missing for fifo insert victim",
            ))?;
        Ok(entry.state.load_state == LoadState::Ready
            && entry.state.dirty_ranges.is_empty()
            && entry.state.active_snapshot.is_none()
            && entry.state.pending_patches.is_empty())
    }

    fn can_evict_lru_head_for_promotion(&self) -> Result<bool, CacheError> {
        let block_index = self
            .lru
            .front_value()
            .ok_or(CacheError::InvariantViolation(
                "lru missing head while promotion path reports full",
            ))?;
        let entry = self
            .blocks
            .get(&block_index)
            .ok_or(CacheError::InvariantViolation(
                "resident block missing for lru promotion victim",
            ))?;
        Ok(entry.state.load_state == LoadState::Ready
            && entry.state.dirty_ranges.is_empty()
            && entry.state.active_snapshot.is_none()
            && entry.state.pending_patches.is_empty())
    }

    fn evict_fifo_head(&mut self) -> Result<Option<EvictedBlock>, CacheError> {
        let block_index = match self.fifo.pop_front() {
            Some(block_index) => block_index,
            None => return Ok(None),
        };

        let entry = self
            .blocks
            .remove(&block_index)
            .ok_or(CacheError::InvariantViolation(
                "resident block missing after fifo head eviction",
            ))?;
        Ok(Some(EvictedBlock { block_index, entry }))
    }

    fn evict_lru_head(&mut self) -> Result<Option<EvictedBlock>, CacheError> {
        let block_index = match self.lru.pop_front() {
            Some(block_index) => block_index,
            None => return Ok(None),
        };

        let entry = self
            .blocks
            .remove(&block_index)
            .ok_or(CacheError::InvariantViolation(
                "resident block missing after lru head eviction",
            ))?;
        Ok(Some(EvictedBlock { block_index, entry }))
    }

    fn apply_pending_patches(entry: &mut BlockEntry) -> Result<(), CacheError> {
        let pending_patches = std::mem::take(&mut entry.state.pending_patches);
        for patch in pending_patches {
            Self::apply_patch(entry, patch.range, &patch.data)?;
        }
        Ok(())
    }

    fn apply_patch(
        entry: &mut BlockEntry,
        range: Range<usize>,
        data: &[u8],
    ) -> Result<(), CacheError> {
        Self::validate_patch(&range, data.len(), entry.data.len())?;
        entry.data[range.clone()].copy_from_slice(data);
        entry.state.dirty_ranges.push(range);
        Ok(())
    }

    fn validate_patch(
        range: &Range<usize>,
        data_len: usize,
        block_len: usize,
    ) -> Result<(), CacheError> {
        if range.start > range.end || range.end > block_len {
            return Err(CacheError::InvariantViolation(
                "resident patch range exceeds block bounds",
            ));
        }
        if data_len != range.end - range.start {
            return Err(CacheError::InvariantViolation(
                "resident patch data length does not match range",
            ));
        }

        Ok(())
    }
}

#[cfg(test)]
mod tests {
    use super::{BeginLoadResult, LoadState, QueueRef, TwoQueueResident};

    fn resident(fifo_capacity: usize, lru_capacity: usize) -> TwoQueueResident {
        TwoQueueResident::new(fifo_capacity, lru_capacity, 8).unwrap()
    }

    fn block(fill: u8) -> Vec<u8> {
        vec![fill; 8]
    }

    #[test]
    fn insert_ready_puts_new_block_into_fifo() {
        let mut resident = resident(2, 2);
        let result = resident.insert_ready(7, block(7), 8).unwrap();

        assert!(result.evicted.is_none());
        assert_eq!(resident.fifo_values(), vec![7]);
        assert!(resident.lru_values().is_empty());
        assert_eq!(resident.len(), 1);

        let entry = resident.get(7).unwrap();
        assert_eq!(entry.state.load_state, LoadState::Ready);
        assert_eq!(entry.state.valid_len, 8);
        assert!(entry.state.dirty_ranges.is_empty());
        assert!(entry.state.active_snapshot.is_none());
        assert!(entry.state.pending_patches.is_empty());
        assert!(matches!(entry.state.queue_ref, QueueRef::Fifo(_)));
    }

    #[test]
    fn fifo_hit_promotes_block_to_lru() {
        let mut resident = resident(2, 2);
        resident.insert_ready(7, block(7), 8).unwrap();

        let result = resident.record_hit(7).unwrap().unwrap();
        assert!(result.evicted.is_none());
        assert!(result.promoted);
        assert!(resident.fifo_values().is_empty());
        assert_eq!(resident.lru_values(), vec![7]);
        assert!(matches!(
            resident.get(7).unwrap().state.queue_ref,
            QueueRef::Lru(_)
        ));
    }

    #[test]
    fn lru_hit_moves_block_to_tail() {
        let mut resident = resident(2, 2);
        resident.insert_ready(1, block(1), 8).unwrap();
        resident.record_hit(1).unwrap();
        resident.insert_ready(2, block(2), 8).unwrap();
        resident.record_hit(2).unwrap();

        assert_eq!(resident.lru_values(), vec![1, 2]);
        let result = resident.record_hit(1).unwrap().unwrap();

        assert!(!result.promoted);
        assert!(result.evicted.is_none());
        assert_eq!(resident.lru_values(), vec![2, 1]);
    }

    #[test]
    fn insert_ready_evicts_fifo_head_when_fifo_is_full() {
        let mut resident = resident(2, 2);
        resident.insert_ready(1, block(1), 8).unwrap();
        resident.insert_ready(2, block(2), 8).unwrap();

        assert_eq!(resident.select_insert_victim(), Some(1));
        let result = resident.insert_ready(3, block(3), 8).unwrap();

        assert_eq!(result.evicted.unwrap().block_index, 1);
        assert_eq!(resident.fifo_values(), vec![2, 3]);
        assert!(resident.get(1).is_none());
        assert_eq!(resident.len(), 2);
    }

    #[test]
    fn promotion_evicts_lru_head_when_lru_is_full() {
        let mut resident = resident(2, 1);
        resident.insert_ready(1, block(1), 8).unwrap();
        resident.record_hit(1).unwrap();
        resident.insert_ready(2, block(2), 8).unwrap();

        assert_eq!(resident.select_promotion_victim(), Some(1));
        let result = resident.record_hit(2).unwrap().unwrap();

        assert!(result.promoted);
        assert_eq!(result.evicted.unwrap().block_index, 1);
        assert_eq!(resident.lru_values(), vec![2]);
        assert!(resident.get(1).is_none());
    }

    #[test]
    fn fifo_hit_does_not_evict_dirty_lru_head() {
        let mut resident = resident(2, 1);
        resident.insert_ready(1, block(1), 8).unwrap();
        resident.record_hit(1).unwrap();
        resident.patch_ready_block(1, 0..1, &[9]).unwrap();
        resident.insert_ready(2, block(2), 8).unwrap();

        let result = resident.record_hit(2).unwrap().unwrap();

        assert!(!result.promoted);
        assert!(result.evicted.is_none());
        assert_eq!(resident.fifo_values(), vec![2]);
        assert_eq!(resident.lru_values(), vec![1]);
        assert_eq!(
            &resident.get(1).unwrap().data[..],
            &[9, 1, 1, 1, 1, 1, 1, 1]
        );
    }

    #[test]
    fn promotion_does_not_copy_block_data() {
        let mut resident = resident(2, 2);
        resident.insert_ready(9, block(9), 8).unwrap();
        let before_ptr = resident.get(9).unwrap().data.as_ptr();

        resident.record_hit(9).unwrap();
        let after_ptr = resident.get(9).unwrap().data.as_ptr();

        assert_eq!(before_ptr, after_ptr);
    }

    #[test]
    fn evict_block_uses_queue_ref_without_scanning() {
        let mut resident = resident(2, 2);
        resident.insert_ready(5, block(5), 8).unwrap();
        resident.record_hit(5).unwrap();

        let evicted = resident.evict_block(5).unwrap().unwrap();
        assert_eq!(evicted.block_index, 5);
        assert!(resident.get(5).is_none());
        assert!(resident.fifo_values().is_empty());
        assert!(resident.lru_values().is_empty());
    }

    #[test]
    fn begin_remote_load_inserts_loading_placeholder_into_fifo() {
        let mut resident = resident(2, 2);

        let result = resident.begin_remote_load(11, 8).unwrap();

        assert_eq!(result, BeginLoadResult::Started);
        assert_eq!(resident.fifo_values(), vec![11]);
        let entry = resident.get(11).unwrap();
        assert_eq!(entry.state.load_state, LoadState::LoadingRemote);
        assert!(matches!(entry.state.queue_ref, QueueRef::Fifo(_)));
    }

    #[test]
    fn begin_remote_load_waits_when_fifo_head_is_still_loading() {
        let mut resident = resident(1, 2);
        assert_eq!(
            resident.begin_remote_load(1, 8).unwrap(),
            BeginLoadResult::Started
        );

        let result = resident.begin_remote_load(2, 8).unwrap();

        assert_eq!(result, BeginLoadResult::Wait);
        assert_eq!(resident.fifo_values(), vec![1]);
        assert!(resident.get(2).is_none());
    }

    #[test]
    fn finish_remote_load_marks_placeholder_ready() {
        let mut resident = resident(2, 2);
        resident.begin_remote_load(4, 8).unwrap();

        resident.finish_remote_load(4, block(4)).unwrap();

        let entry = resident.get(4).unwrap();
        assert_eq!(entry.state.load_state, LoadState::Ready);
        assert_eq!(&entry.data[..], &[4; 8]);
    }

    #[test]
    fn finish_remote_load_applies_pending_patches_and_marks_dirty() {
        let mut resident = resident(2, 2);
        resident.begin_remote_load(4, 8).unwrap();
        resident.enqueue_pending_patch(4, 2..5, &[9, 8, 7]).unwrap();

        resident.finish_remote_load(4, block(4)).unwrap();

        let entry = resident.get(4).unwrap();
        assert_eq!(entry.state.load_state, LoadState::Ready);
        assert_eq!(&entry.data[..], &[4, 4, 9, 8, 7, 4, 4, 4]);
        assert_eq!(entry.state.dirty_ranges, vec![2..5]);
        assert!(entry.state.pending_patches.is_empty());
    }

    #[test]
    fn patch_ready_block_updates_data_without_copying_resident() {
        let mut resident = resident(2, 2);
        resident.insert_ready(3, block(3), 8).unwrap();
        let before_ptr = resident.get(3).unwrap().data.as_ptr();

        resident.patch_ready_block(3, 1..4, &[7, 8, 9]).unwrap();

        let entry = resident.get(3).unwrap();
        assert_eq!(before_ptr, entry.data.as_ptr());
        assert_eq!(&entry.data[..], &[3, 7, 8, 9, 3, 3, 3, 3]);
        assert_eq!(entry.state.dirty_ranges, vec![1..4]);
    }

    #[test]
    fn abort_remote_load_removes_loading_placeholder() {
        let mut resident = resident(2, 2);
        resident.begin_remote_load(6, 8).unwrap();

        resident.abort_remote_load(6).unwrap();

        assert!(resident.get(6).is_none());
        assert!(resident.fifo_values().is_empty());
    }
}
