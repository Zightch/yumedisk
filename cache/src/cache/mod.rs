mod right_io;
mod state;
#[cfg(test)]
mod tests;
mod worker;

use std::collections::{HashMap, HashSet};
use std::sync::{Arc, Condvar, Mutex, MutexGuard};
use std::thread::JoinHandle;
use std::time::{Duration, Instant};

use self::state::{
    CacheState, PrepareInsertResult, ReadBlockAction, SpilledDirty, WaitReason, WriteBlockAction,
};
use crate::block::{BlockLayout, TouchedBlock};
use crate::deps::CacheDeps;
use crate::resident::{LoadState, TwoQueueResident};
#[cfg(test)]
use crate::temp::TempStore;
#[cfg(any(test, feature = "test-hooks"))]
use crate::test_support::{
    CacheSnapshot, HookPoint, SpilledDirtySnapshot, TempFailureController, TestHooks,
};
use crate::{AtIo, CacheConfig, CacheError};

#[derive(Debug)]
pub struct Cache<R> {
    config: CacheConfig,
    layout: BlockLayout,
    state: Arc<Mutex<CacheState>>,
    state_changed: Arc<Condvar>,
    deps: CacheDeps,
    right: Arc<R>,
    worker: Mutex<Option<JoinHandle<()>>>,
}

impl<R: AtIo + 'static> Cache<R> {
    pub fn new(config: CacheConfig, right: R) -> Result<Self, CacheError> {
        let deps = CacheDeps::new(config.temp_dir.clone());
        Self::build(config, right, deps)
    }

    #[cfg(any(test, feature = "test-hooks"))]
    pub fn new_for_test(
        config: CacheConfig,
        right: R,
        hooks: TestHooks,
    ) -> Result<Self, CacheError> {
        let deps = CacheDeps::new(config.temp_dir.clone()).with_test_hooks(hooks);
        Self::build(config, right, deps)
    }

    #[cfg(any(test, feature = "test-hooks"))]
    pub fn new_for_test_with_temp_failures(
        config: CacheConfig,
        right: R,
        hooks: TestHooks,
        temp_failures: TempFailureController,
    ) -> Result<Self, CacheError> {
        let deps = CacheDeps::new(config.temp_dir.clone())
            .with_test_hooks(hooks)
            .with_temp_failures(temp_failures);
        Self::build(config, right, deps)
    }

    fn build(config: CacheConfig, right: R, deps: CacheDeps) -> Result<Self, CacheError> {
        if config.temp_max_files == 0 {
            return Err(CacheError::InvalidConfig(
                "temp_max_files must be greater than 0",
            ));
        }

        let layout = BlockLayout::new(config.block_size_bytes)?;
        let state = Arc::new(Mutex::new(CacheState {
            resident: TwoQueueResident::new(
                config.fifo_capacity_blocks,
                config.lru_capacity_blocks,
                layout.block_size_usize()?,
            )?,
            spilled_dirty: HashMap::new(),
            active_temp_blocks: HashSet::new(),
            foreground_dirty_eviction_waiters: 0,
            stop_requested: false,
        }));
        let state_changed = Arc::new(Condvar::new());
        let right = Arc::new(right);
        let worker = Self::spawn_flush_worker(
            config.clone(),
            layout.clone(),
            Arc::clone(&state),
            Arc::clone(&state_changed),
            deps.temp_clone(),
            Arc::clone(&right),
            #[cfg(any(test, feature = "test-hooks"))]
            deps.hooks().clone(),
        );
        Ok(Self {
            deps,
            config,
            layout,
            state,
            state_changed,
            right,
            worker: Mutex::new(Some(worker)),
        })
    }

    pub fn config(&self) -> &CacheConfig {
        &self.config
    }

    pub fn right(&self) -> &R {
        self.right.as_ref()
    }

    pub fn wait_for_quiesce(&self, timeout: Duration) -> Result<(), CacheError> {
        let deadline = Instant::now()
            .checked_add(timeout)
            .ok_or(CacheError::TimedOut {
                operation: "cache quiesce",
                timeout,
            })?;
        let mut state = self.state.lock().unwrap();
        loop {
            if state.is_quiescent() {
                return Ok(());
            }

            let now = Instant::now();
            if now >= deadline {
                return Err(CacheError::TimedOut {
                    operation: "cache quiesce",
                    timeout,
                });
            }

            let remaining = deadline.saturating_duration_since(now);
            let wait_result = self.state_changed.wait_timeout(state, remaining).unwrap();
            state = wait_result.0;
            if wait_result.1.timed_out() && !state.is_quiescent() {
                return Err(CacheError::TimedOut {
                    operation: "cache quiesce",
                    timeout,
                });
            }
        }
    }

    pub fn read_locked(&self, offset: u64, buffer: &mut [u8]) -> Result<(), CacheError> {
        let touched_blocks = self.layout.touched_blocks(offset, buffer.len())?;
        let block_size = self.layout.block_size_usize()?;

        for touched in touched_blocks {
            self.read_touched_block(&touched, buffer, block_size)?;
        }

        Ok(())
    }

    pub fn write_locked(&self, offset: u64, data: &[u8]) -> Result<(), CacheError> {
        let touched_blocks = self.layout.touched_blocks(offset, data.len())?;
        let block_size = self.layout.block_size_usize()?;

        for touched in touched_blocks {
            self.write_touched_block(&touched, data, block_size)?;
        }

        Ok(())
    }

    #[allow(dead_code)]
    pub(crate) fn layout(&self) -> &BlockLayout {
        &self.layout
    }

    #[cfg(any(test, feature = "test-hooks"))]
    pub fn debug_snapshot(&self) -> CacheSnapshot {
        let snapshot = {
            let state = self.state.lock().unwrap();
            let mut spilled_dirty = state
                .spilled_dirty
                .iter()
                .map(|(&block_index, spilled)| SpilledDirtySnapshot {
                    block_index,
                    valid_len: spilled.valid_len,
                })
                .collect::<Vec<_>>();
            spilled_dirty.sort_by_key(|block| block.block_index);

            let mut active_temp_blocks =
                state.active_temp_blocks.iter().copied().collect::<Vec<_>>();
            active_temp_blocks.sort_unstable();

            CacheSnapshot {
                resident: state.resident.debug_snapshot(),
                spilled_dirty,
                active_temp_blocks,
                foreground_dirty_eviction_waiters: state.foreground_dirty_eviction_waiters,
                stop_requested: state.stop_requested,
            }
        };
        self.deps
            .hooks()
            .observe_state(HookPoint::DebugSnapshot, &snapshot);
        snapshot
    }

    #[cfg(test)]
    fn temp_store(&self) -> &TempStore {
        self.deps.temp()
    }

    fn read_touched_block(
        &self,
        touched: &TouchedBlock,
        buffer: &mut [u8],
        valid_len: usize,
    ) -> Result<(), CacheError> {
        let mut waiting_for_temp_slot = false;
        loop {
            let mut state = self.state.lock().unwrap();
            if state.stop_requested {
                Self::release_foreground_dirty_waiter(&mut state, &mut waiting_for_temp_slot)?;
                return Err(CacheError::Stopped);
            }
            let action = match self.plan_read_touched_block(&mut state, touched, buffer, valid_len)
            {
                Ok(action) => action,
                Err(error) => {
                    Self::release_foreground_dirty_waiter(&mut state, &mut waiting_for_temp_slot)?;
                    return Err(error);
                }
            };

            match action {
                ReadBlockAction::Copied => {
                    Self::release_foreground_dirty_waiter(&mut state, &mut waiting_for_temp_slot)?;
                    return Ok(());
                }
                ReadBlockAction::Wait(reason) => {
                    self.wait_for_state_change(state, &mut waiting_for_temp_slot, reason)?;
                }
                ReadBlockAction::LoadRemote {
                    block_index,
                    block_base,
                } => {
                    Self::release_foreground_dirty_waiter(&mut state, &mut waiting_for_temp_slot)?;
                    drop(state);
                    let mut loaded_block = vec![0u8; valid_len];
                    let read_result = self.read_right_block(block_base, &mut loaded_block);

                    let mut state = self.state.lock().unwrap();
                    match read_result {
                        Ok(()) => {
                            state
                                .resident
                                .finish_remote_load(block_index, loaded_block)?;
                            self.copy_ready_block(&state, touched, buffer)?;
                            drop(state);
                            self.state_changed.notify_all();
                            return Ok(());
                        }
                        Err(read_error) => match state.resident.abort_remote_load(block_index) {
                            Ok(()) => {
                                drop(state);
                                self.state_changed.notify_all();
                                return Err(read_error);
                            }
                            Err(rollback_error) => {
                                drop(state);
                                self.state_changed.notify_all();
                                return Err(rollback_error);
                            }
                        },
                    }
                }
                ReadBlockAction::Rehydrate { block_index } => {
                    Self::release_foreground_dirty_waiter(&mut state, &mut waiting_for_temp_slot)?;
                    drop(state);
                    let mut loaded_block = vec![0u8; valid_len];
                    let read_result = self.read_temp_block(block_index, &mut loaded_block);

                    let mut state = self.state.lock().unwrap();
                    let removed = state.active_temp_blocks.remove(&block_index);
                    debug_assert!(removed);
                    match read_result {
                        Ok(()) => {
                            self.finish_rehydrate(&mut state, block_index, loaded_block)?;
                            self.copy_ready_block(&state, touched, buffer)?;
                            drop(state);
                            self.state_changed.notify_all();
                            return Ok(());
                        }
                        Err(read_error) => match state.resident.abort_remote_load(block_index) {
                            Ok(()) => {
                                drop(state);
                                self.state_changed.notify_all();
                                return Err(read_error);
                            }
                            Err(rollback_error) => {
                                drop(state);
                                self.state_changed.notify_all();
                                return Err(rollback_error);
                            }
                        },
                    }
                }
            }
        }
    }

    fn write_touched_block(
        &self,
        touched: &TouchedBlock,
        data: &[u8],
        valid_len: usize,
    ) -> Result<(), CacheError> {
        let mut waiting_for_temp_slot = false;
        loop {
            let mut state = self.state.lock().unwrap();
            if state.stop_requested {
                Self::release_foreground_dirty_waiter(&mut state, &mut waiting_for_temp_slot)?;
                return Err(CacheError::Stopped);
            }
            let action = match self.plan_write_touched_block(&mut state, touched, data, valid_len) {
                Ok(action) => action,
                Err(error) => {
                    Self::release_foreground_dirty_waiter(&mut state, &mut waiting_for_temp_slot)?;
                    return Err(error);
                }
            };

            match action {
                WriteBlockAction::Patched => {
                    Self::release_foreground_dirty_waiter(&mut state, &mut waiting_for_temp_slot)?;
                    return Ok(());
                }
                WriteBlockAction::Wait(reason) => {
                    self.wait_for_state_change(state, &mut waiting_for_temp_slot, reason)?;
                }
                WriteBlockAction::LoadRemote {
                    block_index,
                    block_base,
                } => {
                    Self::release_foreground_dirty_waiter(&mut state, &mut waiting_for_temp_slot)?;
                    drop(state);
                    let mut loaded_block = vec![0u8; valid_len];
                    let read_result = self.read_right_block(block_base, &mut loaded_block);

                    let mut state = self.state.lock().unwrap();
                    match read_result {
                        Ok(()) => {
                            state
                                .resident
                                .finish_remote_load(block_index, loaded_block)?;
                            drop(state);
                            self.state_changed.notify_all();
                            return Ok(());
                        }
                        Err(read_error) => match state.resident.abort_remote_load(block_index) {
                            Ok(()) => {
                                drop(state);
                                self.state_changed.notify_all();
                                return Err(read_error);
                            }
                            Err(rollback_error) => {
                                drop(state);
                                self.state_changed.notify_all();
                                return Err(rollback_error);
                            }
                        },
                    }
                }
                WriteBlockAction::Rehydrate { block_index } => {
                    Self::release_foreground_dirty_waiter(&mut state, &mut waiting_for_temp_slot)?;
                    drop(state);
                    let mut loaded_block = vec![0u8; valid_len];
                    let read_result = self.read_temp_block(block_index, &mut loaded_block);

                    let mut state = self.state.lock().unwrap();
                    let removed = state.active_temp_blocks.remove(&block_index);
                    debug_assert!(removed);
                    match read_result {
                        Ok(()) => {
                            self.finish_rehydrate(&mut state, block_index, loaded_block)?;
                            drop(state);
                            self.state_changed.notify_all();
                            return Ok(());
                        }
                        Err(read_error) => match state.resident.abort_remote_load(block_index) {
                            Ok(()) => {
                                drop(state);
                                self.state_changed.notify_all();
                                return Err(read_error);
                            }
                            Err(rollback_error) => {
                                drop(state);
                                self.state_changed.notify_all();
                                return Err(rollback_error);
                            }
                        },
                    }
                }
            }
        }
    }

    fn wait_for_state_change(
        &self,
        mut state: MutexGuard<'_, CacheState>,
        waiting_for_temp_slot: &mut bool,
        reason: WaitReason,
    ) -> Result<(), CacheError> {
        match reason {
            WaitReason::Generic => {
                Self::release_foreground_dirty_waiter(&mut state, waiting_for_temp_slot)?;
            }
            WaitReason::DirtyEvictionTempSlot => {
                if !*waiting_for_temp_slot {
                    state.foreground_dirty_eviction_waiters += 1;
                    *waiting_for_temp_slot = true;
                }
            }
        }

        state = self.state_changed.wait(state).unwrap();
        if state.stop_requested {
            Self::release_foreground_dirty_waiter(&mut state, waiting_for_temp_slot)?;
            return Err(CacheError::Stopped);
        }

        Ok(())
    }

    fn release_foreground_dirty_waiter(
        state: &mut CacheState,
        waiting_for_temp_slot: &mut bool,
    ) -> Result<(), CacheError> {
        if !*waiting_for_temp_slot {
            return Ok(());
        }
        if state.foreground_dirty_eviction_waiters == 0 {
            return Err(CacheError::InvariantViolation(
                "foreground dirty eviction waiter count underflow",
            ));
        }

        state.foreground_dirty_eviction_waiters -= 1;
        *waiting_for_temp_slot = false;
        Ok(())
    }

    fn plan_read_touched_block(
        &self,
        state: &mut CacheState,
        touched: &TouchedBlock,
        buffer: &mut [u8],
        valid_len: usize,
    ) -> Result<ReadBlockAction, CacheError> {
        match state
            .resident
            .get(touched.block_index)
            .map(|entry| entry.state.load_state)
        {
            Some(LoadState::Ready) => {
                self.copy_ready_block(state, touched, buffer)?;
                let _ = state.resident.record_hit(touched.block_index)?;
                Ok(ReadBlockAction::Copied)
            }
            Some(LoadState::LoadingRemote | LoadState::Rehydrating) => {
                Ok(ReadBlockAction::Wait(WaitReason::Generic))
            }
            None => {
                self.prepare_read_miss(state, touched.block_index, touched.block_base, valid_len)
            }
        }
    }

    fn plan_write_touched_block(
        &self,
        state: &mut CacheState,
        touched: &TouchedBlock,
        data: &[u8],
        valid_len: usize,
    ) -> Result<WriteBlockAction, CacheError> {
        let patch_slice = self.patch_slice(touched, data)?;
        match state
            .resident
            .get(touched.block_index)
            .map(|entry| entry.state.load_state)
        {
            Some(LoadState::Ready) => {
                state.resident.patch_ready_block(
                    touched.block_index,
                    touched.block_range(),
                    patch_slice,
                )?;
                let _ = state.resident.record_hit(touched.block_index)?;
                Ok(WriteBlockAction::Patched)
            }
            Some(LoadState::LoadingRemote | LoadState::Rehydrating) => {
                Ok(WriteBlockAction::Wait(WaitReason::Generic))
            }
            None => self.prepare_write_miss(
                state,
                touched.block_index,
                touched.block_base,
                touched.block_range(),
                patch_slice,
                valid_len,
            ),
        }
    }

    fn prepare_read_miss(
        &self,
        state: &mut CacheState,
        block_index: u64,
        block_base: u64,
        valid_len: usize,
    ) -> Result<ReadBlockAction, CacheError> {
        if let Some(spilled) = state.spilled_dirty.get(&block_index).copied() {
            if state.active_temp_blocks.contains(&block_index) {
                return Ok(ReadBlockAction::Wait(WaitReason::Generic));
            }
            match self.prepare_resident_slot_for_insert(state)? {
                PrepareInsertResult::Reserved => {
                    let inserted = state.active_temp_blocks.insert(block_index);
                    debug_assert!(inserted);
                    state.resident.insert_loading_placeholder(
                        block_index,
                        spilled.valid_len,
                        LoadState::Rehydrating,
                        None,
                    )?;
                    Ok(ReadBlockAction::Rehydrate { block_index })
                }
                PrepareInsertResult::Wait(reason) => Ok(ReadBlockAction::Wait(reason)),
            }
        } else {
            match self.prepare_resident_slot_for_insert(state)? {
                PrepareInsertResult::Reserved => {
                    state.resident.insert_loading_placeholder(
                        block_index,
                        valid_len,
                        LoadState::LoadingRemote,
                        None,
                    )?;
                    Ok(ReadBlockAction::LoadRemote {
                        block_index,
                        block_base,
                    })
                }
                PrepareInsertResult::Wait(reason) => Ok(ReadBlockAction::Wait(reason)),
            }
        }
    }

    fn prepare_write_miss(
        &self,
        state: &mut CacheState,
        block_index: u64,
        block_base: u64,
        patch_range: std::ops::Range<usize>,
        patch_slice: &[u8],
        valid_len: usize,
    ) -> Result<WriteBlockAction, CacheError> {
        if let Some(spilled) = state.spilled_dirty.get(&block_index).copied() {
            if state.active_temp_blocks.contains(&block_index) {
                return Ok(WriteBlockAction::Wait(WaitReason::Generic));
            }
            match self.prepare_resident_slot_for_insert(state)? {
                PrepareInsertResult::Reserved => {
                    let inserted = state.active_temp_blocks.insert(block_index);
                    debug_assert!(inserted);
                    state.resident.insert_loading_placeholder(
                        block_index,
                        spilled.valid_len,
                        LoadState::Rehydrating,
                        None,
                    )?;
                    state
                        .resident
                        .enqueue_pending_patch(block_index, patch_range, patch_slice)?;
                    Ok(WriteBlockAction::Rehydrate { block_index })
                }
                PrepareInsertResult::Wait(reason) => Ok(WriteBlockAction::Wait(reason)),
            }
        } else {
            match self.prepare_resident_slot_for_insert(state)? {
                PrepareInsertResult::Reserved => {
                    state.resident.insert_loading_placeholder(
                        block_index,
                        valid_len,
                        LoadState::LoadingRemote,
                        None,
                    )?;
                    state
                        .resident
                        .enqueue_pending_patch(block_index, patch_range, patch_slice)?;
                    Ok(WriteBlockAction::LoadRemote {
                        block_index,
                        block_base,
                    })
                }
                PrepareInsertResult::Wait(reason) => Ok(WriteBlockAction::Wait(reason)),
            }
        }
    }

    fn prepare_resident_slot_for_insert(
        &self,
        state: &mut CacheState,
    ) -> Result<PrepareInsertResult, CacheError> {
        let victim_block_index = match state.resident.select_insert_victim() {
            Some(block_index) => block_index,
            None => return Ok(PrepareInsertResult::Reserved),
        };
        if state.active_temp_blocks.contains(&victim_block_index) {
            return Ok(PrepareInsertResult::Wait(WaitReason::Generic));
        }
        let (load_state, has_dirty_ranges, has_active_snapshot, has_pending_patches) =
            {
                let entry = state.resident.get(victim_block_index).ok_or(
                    CacheError::InvariantViolation(
                        "resident victim missing during insert preparation",
                    ),
                )?;
                (
                    entry.state.load_state,
                    !entry.state.dirty_ranges.is_empty(),
                    entry.state.active_snapshot.is_some(),
                    !entry.state.pending_patches.is_empty(),
                )
            };

        if load_state != LoadState::Ready || has_pending_patches {
            return Ok(PrepareInsertResult::Wait(WaitReason::Generic));
        }

        if !has_dirty_ranges && !has_active_snapshot {
            let _ = state.resident.evict_block(victim_block_index)?.ok_or(
                CacheError::InvariantViolation(
                    "resident clean victim missing during insert eviction",
                ),
            )?;
            return Ok(PrepareInsertResult::Reserved);
        }

        if !has_active_snapshot && state.temp_file_count() >= self.config.temp_max_files {
            return Ok(PrepareInsertResult::Wait(WaitReason::DirtyEvictionTempSlot));
        }

        if has_dirty_ranges {
            {
                let entry = state.resident.get(victim_block_index).ok_or(
                    CacheError::InvariantViolation(
                        "resident dirty victim missing during temp spill",
                    ),
                )?;
                #[cfg(any(test, feature = "test-hooks"))]
                self.deps
                    .hooks()
                    .reach_gate(HookPoint::BeforeDirtyVictimSpillTempWrite);
                self.deps
                    .temp()
                    .write_block(victim_block_index, &entry.data)?;
                #[cfg(any(test, feature = "test-hooks"))]
                self.deps
                    .hooks()
                    .reach_gate(HookPoint::AfterDirtyVictimSpillTempWrite);
            }
            state.resident.mark_snapshot_written(victim_block_index)?;
        }

        let evicted = state.resident.evict_block(victim_block_index)?.ok_or(
            CacheError::InvariantViolation("resident dirty victim missing during spill eviction"),
        )?;
        if evicted.entry.state.active_snapshot.is_none() {
            return Err(CacheError::InvariantViolation(
                "dirty spill eviction requires active snapshot",
            ));
        }
        let replaced = state.spilled_dirty.insert(
            victim_block_index,
            SpilledDirty {
                valid_len: evicted.entry.state.valid_len,
            },
        );
        if replaced.is_some() {
            return Err(CacheError::InvariantViolation(
                "spilled dirty block already exists during spill eviction",
            ));
        }

        Ok(PrepareInsertResult::Reserved)
    }

    fn finish_rehydrate(
        &self,
        state: &mut CacheState,
        block_index: u64,
        data: Vec<u8>,
    ) -> Result<(), CacheError> {
        state.resident.finish_remote_load(block_index, data)?;
        let spilled =
            state
                .spilled_dirty
                .remove(&block_index)
                .ok_or(CacheError::InvariantViolation(
                    "spilled dirty block missing during rehydrate completion",
                ))?;
        let resident_valid_len = state
            .resident
            .get(block_index)
            .ok_or(CacheError::InvariantViolation(
                "rehydrated resident block missing after load completion",
            ))?
            .state
            .valid_len;
        if spilled.valid_len != resident_valid_len {
            return Err(CacheError::InvariantViolation(
                "spilled dirty valid_len mismatch during rehydrate completion",
            ));
        }
        state.resident.attach_active_snapshot(block_index)
    }
}

impl<R> Drop for Cache<R> {
    fn drop(&mut self) {
        {
            let mut state = self.state.lock().unwrap();
            state.stop_requested = true;
        }
        self.state_changed.notify_all();
        if let Some(worker) = self.worker.lock().unwrap().take() {
            let _ = worker.join();
        }
    }
}
