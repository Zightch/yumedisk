use std::collections::{HashMap, HashSet};
use std::sync::{Arc, Condvar, Mutex, MutexGuard};
use std::thread::{self, JoinHandle};
use std::time::Duration;

use crate::block::{BlockLayout, TouchedBlock};
use crate::deps::CacheDeps;
use crate::resident::{LoadState, TwoQueueResident};
use crate::temp::TempStore;
#[cfg(any(test, feature = "test-hooks"))]
use crate::test_support::{CacheSnapshot, HookPoint, SpilledDirtySnapshot, TestHooks};
use crate::{AtIo, CacheConfig, CacheError};

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
enum ReadBlockAction {
    Copied,
    LoadRemote { block_index: u64, block_base: u64 },
    Rehydrate { block_index: u64 },
    Wait(WaitReason),
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
enum WriteBlockAction {
    Patched,
    LoadRemote { block_index: u64, block_base: u64 },
    Rehydrate { block_index: u64 },
    Wait(WaitReason),
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
enum WaitReason {
    Generic,
    DirtyEvictionTempSlot,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
struct SpilledDirty {
    valid_len: usize,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
enum PrepareInsertResult {
    Reserved,
    Wait(WaitReason),
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
enum WorkerAction {
    FlushTemp { block_index: u64, block_base: u64 },
    SnapshotCreated,
}

#[derive(Debug)]
struct CacheState {
    resident: TwoQueueResident,
    spilled_dirty: HashMap<u64, SpilledDirty>,
    active_temp_blocks: HashSet<u64>,
    foreground_dirty_eviction_waiters: usize,
    stop_requested: bool,
}

impl CacheState {
    fn temp_file_count(&self) -> usize {
        self.spilled_dirty.len() + self.resident.active_snapshot_count()
    }
}

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

    fn spawn_flush_worker(
        config: CacheConfig,
        layout: BlockLayout,
        state: Arc<Mutex<CacheState>>,
        state_changed: Arc<Condvar>,
        temp: TempStore,
        right: Arc<R>,
    ) -> JoinHandle<()> {
        thread::Builder::new()
            .name("cache-flush-worker".into())
            .spawn(move || {
                Self::run_flush_worker(config, layout, state, state_changed, temp, right);
            })
            .expect("cache flush worker thread must start")
    }

    fn run_flush_worker(
        config: CacheConfig,
        layout: BlockLayout,
        state: Arc<Mutex<CacheState>>,
        state_changed: Arc<Condvar>,
        temp: TempStore,
        right: Arc<R>,
    ) {
        let wait_duration = Self::worker_wait_duration(config.dirty_scan_interval);
        let mut scan_due = false;

        loop {
            let action = {
                let mut state_guard = state.lock().unwrap();
                loop {
                    if state_guard.stop_requested {
                        return;
                    }
                    if scan_due {
                        if let Some(action) =
                            Self::select_existing_temp_flush(&layout, &mut state_guard)
                                .expect("cache worker temp flush planning must preserve invariants")
                        {
                            break action;
                        }
                        if Self::create_snapshot_temp_locked(&config, &mut state_guard, &temp)
                            .expect("cache worker snapshot creation must preserve invariants")
                        {
                            break WorkerAction::SnapshotCreated;
                        }
                        scan_due = false;
                    }

                    let wait_result = state_changed
                        .wait_timeout(state_guard, wait_duration)
                        .unwrap();
                    state_guard = wait_result.0;
                    if wait_result.1.timed_out() {
                        scan_due = true;
                    }
                }
            };

            match action {
                WorkerAction::FlushTemp {
                    block_index,
                    block_base,
                } => {
                    Self::flush_active_temp(
                        &layout,
                        &state,
                        &state_changed,
                        &temp,
                        right.as_ref(),
                        block_index,
                        block_base,
                    )
                    .expect("cache worker temp flush must preserve invariants");
                }
                WorkerAction::SnapshotCreated => {
                    state_changed.notify_all();
                }
            }
        }
    }

    fn worker_wait_duration(interval: Duration) -> Duration {
        if interval.is_zero() {
            Duration::from_millis(1)
        } else {
            interval
        }
    }

    fn select_existing_temp_flush(
        layout: &BlockLayout,
        state: &mut CacheState,
    ) -> Result<Option<WorkerAction>, CacheError> {
        let spilled_block_index = state
            .spilled_dirty
            .keys()
            .copied()
            .filter(|block_index| !state.active_temp_blocks.contains(block_index))
            .min();
        if let Some(block_index) = spilled_block_index {
            let inserted = state.active_temp_blocks.insert(block_index);
            debug_assert!(inserted);
            return Ok(Some(WorkerAction::FlushTemp {
                block_index,
                block_base: layout.block_base(block_index)?,
            }));
        }

        let resident_block_index = state.resident.select_active_snapshot_block(|block_index| {
            !state.active_temp_blocks.contains(&block_index)
        });
        if let Some(block_index) = resident_block_index {
            let inserted = state.active_temp_blocks.insert(block_index);
            debug_assert!(inserted);
            return Ok(Some(WorkerAction::FlushTemp {
                block_index,
                block_base: layout.block_base(block_index)?,
            }));
        }

        Ok(None)
    }

    fn create_snapshot_temp_locked(
        config: &CacheConfig,
        state: &mut CacheState,
        temp: &TempStore,
    ) -> Result<bool, CacheError> {
        if state.foreground_dirty_eviction_waiters > 0 {
            return Ok(false);
        }
        if state.temp_file_count() >= config.temp_max_files {
            return Ok(false);
        }

        let block_index = match state.resident.select_snapshot_candidate(|_| true) {
            Some(block_index) => block_index,
            None => return Ok(false),
        };
        {
            let entry = state
                .resident
                .get(block_index)
                .ok_or(CacheError::InvariantViolation(
                    "resident dirty block missing during worker snapshot creation",
                ))?;
            if entry.state.load_state != LoadState::Ready {
                return Err(CacheError::InvariantViolation(
                    "worker snapshot creation requires ready resident block",
                ));
            }
            if entry.state.active_snapshot.is_some() {
                return Err(CacheError::InvariantViolation(
                    "worker snapshot creation requires resident block without active snapshot",
                ));
            }
            if entry.state.dirty_ranges.is_empty() {
                return Err(CacheError::InvariantViolation(
                    "worker snapshot creation requires resident dirty block",
                ));
            }
            if !entry.state.pending_patches.is_empty() {
                return Err(CacheError::InvariantViolation(
                    "worker snapshot creation requires resident block without pending patches",
                ));
            }
            if temp.write_block(block_index, &entry.data).is_err() {
                return Ok(false);
            }
        }
        state.resident.mark_snapshot_written(block_index)?;

        Ok(true)
    }

    fn flush_active_temp(
        layout: &BlockLayout,
        state: &Arc<Mutex<CacheState>>,
        state_changed: &Arc<Condvar>,
        temp: &TempStore,
        right: &R,
        block_index: u64,
        block_base: u64,
    ) -> Result<(), CacheError> {
        let mut temp_buffer = vec![0u8; layout.block_size_usize()?];
        let flush_result = temp
            .read_block(block_index, &mut temp_buffer)
            .and_then(|_| {
                layout.validate_right_io(block_base, temp_buffer.len())?;
                right.write_at(block_base, &temp_buffer)
            })
            .and_then(|_| temp.remove_block(block_index));

        let mut state_guard = state.lock().unwrap();
        let removed = state_guard.active_temp_blocks.remove(&block_index);
        debug_assert!(removed);
        if flush_result.is_err() {
            drop(state_guard);
            state_changed.notify_all();
            return Ok(());
        }
        Self::finish_flush_success(&mut state_guard, block_index)?;

        drop(state_guard);
        state_changed.notify_all();
        Ok(())
    }

    fn finish_flush_success(state: &mut CacheState, block_index: u64) -> Result<(), CacheError> {
        if state.spilled_dirty.remove(&block_index).is_some() {
            return Ok(());
        }

        if state.resident.get(block_index).is_some() {
            return state.resident.clear_active_snapshot(block_index);
        }

        Err(CacheError::InvariantViolation(
            "flushed temp block missing from resident and spilled tables",
        ))
    }

    fn copy_ready_block(
        &self,
        state: &CacheState,
        touched: &TouchedBlock,
        buffer: &mut [u8],
    ) -> Result<(), CacheError> {
        let entry =
            state
                .resident
                .get(touched.block_index)
                .ok_or(CacheError::InvariantViolation(
                    "resident block missing during read copy",
                ))?;
        if entry.state.load_state != LoadState::Ready {
            return Err(CacheError::InvariantViolation(
                "resident read copy requires ready block",
            ));
        }

        touched.copy_from_block(&entry.data, buffer)
    }

    fn patch_slice<'a>(
        &self,
        touched: &TouchedBlock,
        data: &'a [u8],
    ) -> Result<&'a [u8], CacheError> {
        let range = touched.buffer_range();
        if data.len() < range.end {
            return Err(CacheError::BufferTooSmall {
                context: "request write buffer",
                expected: range.end,
                actual: data.len(),
            });
        }

        Ok(&data[range])
    }

    #[allow(dead_code)]
    fn read_right_block(&self, offset: u64, buffer: &mut [u8]) -> Result<(), CacheError> {
        self.layout.validate_right_io(offset, buffer.len())?;
        #[cfg(any(test, feature = "test-hooks"))]
        self.deps.hooks().reach_gate(HookPoint::BeforeRightRead);
        let result = self.right.read_at(offset, buffer);
        #[cfg(any(test, feature = "test-hooks"))]
        self.deps.hooks().reach_gate(HookPoint::AfterRightRead);
        result
    }

    fn read_temp_block(&self, block_index: u64, buffer: &mut [u8]) -> Result<(), CacheError> {
        let expected = self.layout.block_size_usize()?;
        if buffer.len() != expected {
            return Err(CacheError::InvalidBlockDataLength {
                expected,
                actual: buffer.len(),
            });
        }

        self.deps.temp().read_block(block_index, buffer)
    }

    #[allow(dead_code)]
    fn write_right_block(&self, offset: u64, data: &[u8]) -> Result<(), CacheError> {
        self.layout.validate_right_io(offset, data.len())?;
        #[cfg(any(test, feature = "test-hooks"))]
        self.deps.hooks().reach_gate(HookPoint::BeforeRightWrite);
        let result = self.right.write_at(offset, data);
        #[cfg(any(test, feature = "test-hooks"))]
        self.deps.hooks().reach_gate(HookPoint::AfterRightWrite);
        result
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

#[cfg(test)]
mod tests {
    use std::sync::{Arc, Mutex, mpsc};
    use std::thread;
    use std::time::Duration;

    use super::{Cache, SpilledDirty};
    use crate::test_support::{
        LoadStateSnapshot, QueueKindSnapshot, TestHooks, TestTempDir, wait_until,
    };
    use crate::{AtIo, CacheConfig, CacheError};

    #[derive(Debug, Clone, PartialEq, Eq)]
    enum IoCall {
        Read { offset: u64, length: usize },
        Write { offset: u64, length: usize },
    }

    #[derive(Debug)]
    struct MockAtIoInner {
        calls: Mutex<Vec<IoCall>>,
        storage: Mutex<Vec<u8>>,
        read_delay: Duration,
        write_delay: Duration,
        write_failures_remaining: Mutex<usize>,
    }

    #[derive(Debug, Clone)]
    struct MockAtIo {
        inner: Arc<MockAtIoInner>,
    }

    impl MockAtIo {
        fn new(
            storage: Vec<u8>,
            read_delay: Duration,
            write_delay: Duration,
            write_failures: usize,
        ) -> Self {
            Self {
                inner: Arc::new(MockAtIoInner {
                    calls: Mutex::new(Vec::new()),
                    storage: Mutex::new(storage),
                    read_delay,
                    write_delay,
                    write_failures_remaining: Mutex::new(write_failures),
                }),
            }
        }

        fn with_len(length: usize) -> Self {
            Self::new(test_bytes(length), Duration::ZERO, Duration::ZERO, 0)
        }

        fn with_delay(length: usize, read_delay: Duration) -> Self {
            Self::new(test_bytes(length), read_delay, Duration::ZERO, 0)
        }

        fn with_write_delay(length: usize, write_delay: Duration) -> Self {
            Self::new(test_bytes(length), Duration::ZERO, write_delay, 0)
        }

        fn take_calls(&self) -> Vec<IoCall> {
            self.inner.calls.lock().unwrap().drain(..).collect()
        }

        fn calls_snapshot(&self) -> Vec<IoCall> {
            self.inner.calls.lock().unwrap().clone()
        }

        fn storage_slice(&self, offset: usize, length: usize) -> Vec<u8> {
            self.inner.storage.lock().unwrap()[offset..offset + length].to_vec()
        }
    }

    impl AtIo for MockAtIo {
        fn read_at(&self, offset: u64, buffer: &mut [u8]) -> Result<(), CacheError> {
            self.inner.calls.lock().unwrap().push(IoCall::Read {
                offset,
                length: buffer.len(),
            });
            if self.inner.read_delay != Duration::ZERO {
                thread::sleep(self.inner.read_delay);
            }

            let start = usize::try_from(offset)
                .map_err(|_| CacheError::ArithmeticOverflow("mock read offset"))?;
            let end = start
                .checked_add(buffer.len())
                .ok_or(CacheError::ArithmeticOverflow("mock read end"))?;
            let storage = self.inner.storage.lock().unwrap();
            buffer.copy_from_slice(&storage[start..end]);
            Ok(())
        }

        fn write_at(&self, offset: u64, data: &[u8]) -> Result<(), CacheError> {
            self.inner.calls.lock().unwrap().push(IoCall::Write {
                offset,
                length: data.len(),
            });
            if self.inner.write_delay != Duration::ZERO {
                thread::sleep(self.inner.write_delay);
            }
            {
                let mut failures_remaining = self.inner.write_failures_remaining.lock().unwrap();
                if *failures_remaining > 0 {
                    *failures_remaining -= 1;
                    return Err(CacheError::NotImplemented);
                }
            }

            let start = usize::try_from(offset)
                .map_err(|_| CacheError::ArithmeticOverflow("mock write offset"))?;
            let end = start
                .checked_add(data.len())
                .ok_or(CacheError::ArithmeticOverflow("mock write end"))?;
            let mut storage = self.inner.storage.lock().unwrap();
            storage[start..end].copy_from_slice(data);
            Ok(())
        }
    }

    impl Default for MockAtIo {
        fn default() -> Self {
            Self::with_len(256)
        }
    }

    fn test_config(block_size_bytes: u32) -> CacheConfig {
        CacheConfig {
            fifo_capacity_blocks: 2,
            lru_capacity_blocks: 2,
            block_size_bytes,
            dirty_scan_interval: Duration::from_secs(1),
            temp_max_files: 1,
            temp_dir: "tmp/cache-tests".into(),
        }
    }

    fn test_bytes(length: usize) -> Vec<u8> {
        (0..length).map(|index| index as u8).collect()
    }

    fn expected_bytes(offset: usize, length: usize) -> Vec<u8> {
        (offset..offset + length).map(|index| index as u8).collect()
    }

    #[test]
    fn new_rejects_zero_block_size() {
        let error = Cache::new(test_config(0), MockAtIo::default()).unwrap_err();
        assert_eq!(
            error,
            CacheError::InvalidConfig("block_size_bytes must be greater than 0")
        );
    }

    #[test]
    fn new_rejects_zero_fifo_capacity() {
        let mut config = test_config(32);
        config.fifo_capacity_blocks = 0;

        let error = Cache::new(config, MockAtIo::default()).unwrap_err();
        assert_eq!(
            error,
            CacheError::InvalidConfig("fifo_capacity_blocks must be greater than 0")
        );
    }

    #[test]
    fn new_rejects_zero_temp_file_limit() {
        let mut config = test_config(32);
        config.temp_max_files = 0;

        let error = Cache::new(config, MockAtIo::default()).unwrap_err();
        assert_eq!(
            error,
            CacheError::InvalidConfig("temp_max_files must be greater than 0")
        );
    }

    #[test]
    fn read_right_block_rejects_misaligned_offset() {
        let cache = Cache::new(test_config(32), MockAtIo::default()).unwrap();
        let mut buffer = [0u8; 32];
        let error = cache.read_right_block(1, &mut buffer).unwrap_err();
        assert_eq!(
            error,
            CacheError::MisalignedRightIo {
                offset: 1,
                length: 32,
                block_size: 32,
            }
        );
    }

    #[test]
    fn read_right_block_rejects_wrong_length() {
        let cache = Cache::new(test_config(32), MockAtIo::default()).unwrap();
        let mut buffer = [0u8; 16];
        let error = cache.read_right_block(32, &mut buffer).unwrap_err();
        assert_eq!(
            error,
            CacheError::MisalignedRightIo {
                offset: 32,
                length: 16,
                block_size: 32,
            }
        );
    }

    #[test]
    fn aligned_right_io_reaches_backend_once() {
        let right = MockAtIo::with_len(160);
        let cache = Cache::new(test_config(32), right.clone()).unwrap();
        let mut read_buffer = [0u8; 32];
        let write_buffer = [7u8; 32];

        cache.read_right_block(64, &mut read_buffer).unwrap();
        cache.write_right_block(96, &write_buffer).unwrap();

        assert_eq!(read_buffer[0], 64);
        assert_eq!(read_buffer[31], 95);
        assert_eq!(
            right.take_calls(),
            vec![
                IoCall::Read {
                    offset: 64,
                    length: 32
                },
                IoCall::Write {
                    offset: 96,
                    length: 32
                }
            ]
        );
    }

    #[test]
    fn debug_snapshot_reports_resident_state_for_test_build() {
        let temp_dir = TestTempDir::new();
        let right = MockAtIo::with_len(128);
        let mut config = test_config(32);
        config.temp_dir = temp_dir.path().to_path_buf();
        let cache = Cache::new_for_test(config, right, TestHooks::default()).unwrap();

        cache.write_locked(4, &[200, 201]).unwrap();

        let snapshot = cache.debug_snapshot();
        assert_eq!(snapshot.resident.fifo_order, vec![0]);
        assert!(snapshot.resident.lru_order.is_empty());
        assert!(snapshot.spilled_dirty.is_empty());
        assert!(snapshot.active_temp_blocks.is_empty());
        assert_eq!(snapshot.foreground_dirty_eviction_waiters, 0);
        assert!(!snapshot.stop_requested);

        let block = snapshot
            .resident
            .blocks
            .iter()
            .find(|block| block.block_index == 0)
            .unwrap();
        assert_eq!(block.queue, QueueKindSnapshot::Fifo);
        assert_eq!(block.load_state, LoadStateSnapshot::Ready);
        assert_eq!(block.dirty_ranges, vec![4..6]);
        assert!(!block.has_active_snapshot);
        assert_eq!(block.pending_patch_count, 0);
        assert_eq!(block.valid_len, 32);
    }

    #[test]
    fn read_locked_hits_resident_after_first_miss() {
        let right = MockAtIo::with_len(128);
        let cache = Cache::new(test_config(32), right.clone()).unwrap();
        let mut first = [0u8; 7];
        let mut second = [0u8; 7];

        cache.read_locked(5, &mut first).unwrap();
        cache.read_locked(5, &mut second).unwrap();

        assert_eq!(&first[..], &expected_bytes(5, 7));
        assert_eq!(&second[..], &expected_bytes(5, 7));
        assert_eq!(
            right.take_calls(),
            vec![IoCall::Read {
                offset: 0,
                length: 32
            }]
        );
    }

    #[test]
    fn read_locked_spans_multiple_blocks_with_aligned_backend_reads() {
        let right = MockAtIo::with_len(160);
        let cache = Cache::new(test_config(32), right.clone()).unwrap();
        let mut buffer = [0u8; 40];

        cache.read_locked(28, &mut buffer).unwrap();

        assert_eq!(&buffer[..], &expected_bytes(28, 40));
        assert_eq!(
            right.take_calls(),
            vec![
                IoCall::Read {
                    offset: 0,
                    length: 32
                },
                IoCall::Read {
                    offset: 32,
                    length: 32
                },
                IoCall::Read {
                    offset: 64,
                    length: 32
                }
            ]
        );
    }

    #[test]
    fn read_locked_reads_tail_slice_from_single_loaded_block() {
        let right = MockAtIo::with_len(128);
        let cache = Cache::new(test_config(32), right.clone()).unwrap();
        let mut buffer = [0u8; 4];

        cache.read_locked(60, &mut buffer).unwrap();

        assert_eq!(&buffer[..], &expected_bytes(60, 4));
        assert_eq!(
            right.take_calls(),
            vec![IoCall::Read {
                offset: 32,
                length: 32
            }]
        );
    }

    #[test]
    fn write_locked_patches_resident_hit_without_backend_write() {
        let right = MockAtIo::with_len(128);
        let cache = Cache::new(test_config(32), right.clone()).unwrap();
        let mut warm = [0u8; 1];
        let mut buffer = [0u8; 8];

        cache.read_locked(0, &mut warm).unwrap();
        cache.write_locked(4, &[200, 201, 202]).unwrap();
        cache.read_locked(0, &mut buffer).unwrap();

        assert_eq!(&buffer[..], &[0, 1, 2, 3, 200, 201, 202, 7]);
        assert_eq!(
            right.take_calls(),
            vec![IoCall::Read {
                offset: 0,
                length: 32
            }]
        );
    }

    #[test]
    fn write_locked_miss_reads_full_block_once_and_patches_resident() {
        let right = MockAtIo::with_len(128);
        let cache = Cache::new(test_config(32), right.clone()).unwrap();
        let mut buffer = [0u8; 10];

        cache.write_locked(5, &[90, 91, 92]).unwrap();
        cache.read_locked(0, &mut buffer).unwrap();

        assert_eq!(&buffer[..], &[0, 1, 2, 3, 4, 90, 91, 92, 8, 9]);
        assert_eq!(
            right.take_calls(),
            vec![IoCall::Read {
                offset: 0,
                length: 32
            }]
        );
    }

    #[test]
    fn write_locked_spans_multiple_blocks_without_backend_write() {
        let right = MockAtIo::with_len(160);
        let mut config = test_config(32);
        config.fifo_capacity_blocks = 4;
        let cache = Cache::new(config, right.clone()).unwrap();
        let patch = vec![0xAB; 40];
        let mut buffer = [0u8; 40];

        cache.write_locked(28, &patch).unwrap();
        cache.read_locked(28, &mut buffer).unwrap();

        assert_eq!(&buffer[..], &patch[..]);
        assert_eq!(
            right.take_calls(),
            vec![
                IoCall::Read {
                    offset: 0,
                    length: 32
                },
                IoCall::Read {
                    offset: 32,
                    length: 32
                },
                IoCall::Read {
                    offset: 64,
                    length: 32
                }
            ]
        );
    }

    #[test]
    fn concurrent_same_block_miss_loads_backend_once() {
        let right = MockAtIo::with_delay(128, Duration::from_millis(100));
        let cache = Arc::new(Cache::new(test_config(32), right.clone()).unwrap());

        let first_cache = Arc::clone(&cache);
        let first = thread::spawn(move || {
            let mut buffer = vec![0u8; 16];
            first_cache.read_locked(3, &mut buffer).unwrap();
            buffer
        });

        thread::sleep(Duration::from_millis(20));

        let second_cache = Arc::clone(&cache);
        let second = thread::spawn(move || {
            let mut buffer = vec![0u8; 8];
            second_cache.read_locked(5, &mut buffer).unwrap();
            buffer
        });

        assert_eq!(first.join().unwrap(), expected_bytes(3, 16));
        assert_eq!(second.join().unwrap(), expected_bytes(5, 8));
        assert_eq!(
            right.take_calls(),
            vec![IoCall::Read {
                offset: 0,
                length: 32
            }]
        );
    }

    #[test]
    fn concurrent_same_block_write_miss_loads_backend_once() {
        let right = MockAtIo::with_delay(128, Duration::from_millis(100));
        let cache = Arc::new(Cache::new(test_config(32), right.clone()).unwrap());

        let first_cache = Arc::clone(&cache);
        let first = thread::spawn(move || {
            first_cache.write_locked(3, &[200, 201, 202, 203]).unwrap();
        });

        thread::sleep(Duration::from_millis(20));

        let second_cache = Arc::clone(&cache);
        let second = thread::spawn(move || {
            second_cache.write_locked(10, &[150, 151, 152]).unwrap();
        });

        first.join().unwrap();
        second.join().unwrap();

        let mut buffer = [0u8; 16];
        cache.read_locked(0, &mut buffer).unwrap();

        assert_eq!(
            &buffer[..],
            &[
                0, 1, 2, 200, 201, 202, 203, 7, 8, 9, 150, 151, 152, 13, 14, 15
            ]
        );
        assert_eq!(
            right.take_calls(),
            vec![IoCall::Read {
                offset: 0,
                length: 32
            }]
        );
    }

    #[test]
    fn write_locked_waits_for_read_load_without_second_backend_read() {
        let right = MockAtIo::with_delay(128, Duration::from_millis(100));
        let cache = Arc::new(Cache::new(test_config(32), right.clone()).unwrap());

        let first_cache = Arc::clone(&cache);
        let first = thread::spawn(move || {
            let mut buffer = vec![0u8; 8];
            first_cache.read_locked(2, &mut buffer).unwrap();
            buffer
        });

        thread::sleep(Duration::from_millis(20));

        let second_cache = Arc::clone(&cache);
        let second = thread::spawn(move || {
            second_cache.write_locked(6, &[222, 223, 224]).unwrap();
        });

        assert_eq!(first.join().unwrap(), expected_bytes(2, 8));
        second.join().unwrap();

        let mut buffer = [0u8; 12];
        cache.read_locked(0, &mut buffer).unwrap();

        assert_eq!(&buffer[..], &[0, 1, 2, 3, 4, 5, 222, 223, 224, 9, 10, 11]);
        assert_eq!(
            right.take_calls(),
            vec![IoCall::Read {
                offset: 0,
                length: 32
            }]
        );
    }

    #[test]
    fn dirty_eviction_writes_temp_then_rehydrates_without_second_remote_read() {
        let temp_dir = TestTempDir::new();
        let right = MockAtIo::with_len(128);
        let mut config = test_config(32);
        config.fifo_capacity_blocks = 1;
        config.dirty_scan_interval = Duration::from_secs(60);
        config.temp_dir = temp_dir.path().to_path_buf();
        let cache = Cache::new(config, right.clone()).unwrap();
        let temp_path = cache.temp_store().path_for_block(0);

        cache.write_locked(4, &[200, 201, 202]).unwrap();
        cache.read_locked(32, &mut [0u8; 1]).unwrap();

        assert!(temp_path.is_file());
        {
            let state = cache.state.lock().unwrap();
            assert!(state.resident.get(0).is_none());
            assert_eq!(
                state.spilled_dirty.get(&0),
                Some(&SpilledDirty { valid_len: 32 })
            );
        }

        let mut buffer = [0u8; 8];
        cache.read_locked(0, &mut buffer).unwrap();

        assert_eq!(&buffer[..], &[0, 1, 2, 3, 200, 201, 202, 7]);
        assert!(temp_path.is_file());
        {
            let state = cache.state.lock().unwrap();
            let entry = state.resident.get(0).unwrap();
            assert!(entry.state.active_snapshot.is_some());
            assert!(entry.state.dirty_ranges.is_empty());
            assert!(!state.spilled_dirty.contains_key(&0));
        }
        assert_eq!(
            right.take_calls(),
            vec![
                IoCall::Read {
                    offset: 0,
                    length: 32
                },
                IoCall::Read {
                    offset: 32,
                    length: 32
                }
            ]
        );
    }

    #[test]
    fn dirty_eviction_preserves_dirty_ranges_when_temp_write_fails() {
        let temp_dir = TestTempDir::new();
        let right = MockAtIo::with_len(128);
        let mut config = test_config(32);
        config.fifo_capacity_blocks = 1;
        config.dirty_scan_interval = Duration::from_secs(60);
        config.temp_dir = temp_dir.child("missing-temp-root");
        let cache = Cache::new(config, right).unwrap();

        cache.write_locked(4, &[200, 201, 202]).unwrap();
        let error = cache.read_locked(32, &mut [0u8; 1]).unwrap_err();

        match error {
            CacheError::TempIo { kind, .. } => {
                assert_eq!(kind, std::io::ErrorKind::NotFound);
            }
            other => panic!("unexpected error: {other:?}"),
        }

        let state = cache.state.lock().unwrap();
        let entry = state.resident.get(0).unwrap();
        assert_eq!(&entry.data[..8], &[0, 1, 2, 3, 200, 201, 202, 7]);
        assert_eq!(entry.state.dirty_ranges, vec![4..7]);
        assert!(entry.state.active_snapshot.is_none());
        assert!(state.spilled_dirty.is_empty());
        assert!(state.resident.get(1).is_none());
    }

    #[test]
    fn clean_victim_miss_bypasses_full_temp() {
        let temp_dir = TestTempDir::new();
        let right = MockAtIo::with_len(160);
        let mut config = test_config(32);
        config.fifo_capacity_blocks = 1;
        config.dirty_scan_interval = Duration::from_secs(60);
        config.temp_dir = temp_dir.path().to_path_buf();
        let cache = Arc::new(Cache::new(config, right.clone()).unwrap());

        cache.write_locked(4, &[200, 201, 202]).unwrap();
        cache.read_locked(32, &mut [0u8; 1]).unwrap();
        right.take_calls();

        let (tx, rx) = mpsc::channel();
        let read_cache = Arc::clone(&cache);
        let read_thread = thread::spawn(move || {
            let mut buffer = [0u8; 1];
            read_cache.read_locked(64, &mut buffer).unwrap();
            tx.send(buffer[0]).unwrap();
        });

        assert_eq!(rx.recv_timeout(Duration::from_millis(200)).unwrap(), 64);
        read_thread.join().unwrap();

        assert_eq!(
            right.take_calls(),
            vec![IoCall::Read {
                offset: 64,
                length: 32
            }]
        );
        let state = cache.state.lock().unwrap();
        assert_eq!(state.foreground_dirty_eviction_waiters, 0);
        assert!(state.spilled_dirty.contains_key(&0));
        assert!(state.resident.get(1).is_none());
        assert!(state.resident.get(2).is_some());
    }

    #[test]
    fn dirty_victim_miss_waits_for_temp_release_then_loads_remote() {
        let temp_dir = TestTempDir::new();
        let right = MockAtIo::with_len(160);
        let mut config = test_config(32);
        config.fifo_capacity_blocks = 1;
        config.dirty_scan_interval = Duration::from_secs(60);
        config.temp_dir = temp_dir.path().to_path_buf();
        let cache = Arc::new(Cache::new(config, right.clone()).unwrap());

        cache.write_locked(4, &[200, 201, 202]).unwrap();
        cache.write_locked(36, &[210, 211]).unwrap();
        right.take_calls();

        let (tx, rx) = mpsc::channel();
        let read_cache = Arc::clone(&cache);
        let read_thread = thread::spawn(move || {
            let mut buffer = [0u8; 1];
            read_cache.read_locked(64, &mut buffer).unwrap();
            tx.send(buffer[0]).unwrap();
        });

        wait_until(Duration::from_secs(1), || {
            cache
                .state
                .lock()
                .unwrap()
                .foreground_dirty_eviction_waiters
                == 1
        });
        assert!(rx.recv_timeout(Duration::from_millis(100)).is_err());
        assert!(!right.calls_snapshot().iter().any(|call| {
            matches!(
                call,
                IoCall::Read {
                    offset: 64,
                    length: 32
                }
            )
        }));

        {
            let mut state = cache.state.lock().unwrap();
            let removed = state.spilled_dirty.remove(&0);
            assert!(removed.is_some());
        }
        cache.temp_store().remove_block(0).unwrap();
        cache.state_changed.notify_all();

        assert_eq!(rx.recv_timeout(Duration::from_secs(2)).unwrap(), 64);
        read_thread.join().unwrap();

        assert_eq!(
            right.take_calls(),
            vec![IoCall::Read {
                offset: 64,
                length: 32
            }]
        );
        let state = cache.state.lock().unwrap();
        assert_eq!(state.foreground_dirty_eviction_waiters, 0);
        assert_eq!(
            state.spilled_dirty.get(&1),
            Some(&SpilledDirty { valid_len: 32 })
        );
        assert!(state.resident.get(2).is_some());
    }

    #[test]
    fn resident_hits_continue_while_dirty_eviction_waits() {
        let temp_dir = TestTempDir::new();
        let right = MockAtIo::with_len(192);
        let mut config = test_config(32);
        config.fifo_capacity_blocks = 1;
        config.lru_capacity_blocks = 1;
        config.dirty_scan_interval = Duration::from_secs(60);
        config.temp_dir = temp_dir.path().to_path_buf();
        let cache = Arc::new(Cache::new(config, right.clone()).unwrap());

        cache.read_locked(96, &mut [0u8; 1]).unwrap();
        cache.read_locked(96, &mut [0u8; 1]).unwrap();
        cache.write_locked(4, &[200, 201, 202]).unwrap();
        cache.write_locked(36, &[210, 211]).unwrap();
        right.take_calls();

        let waiting_cache = Arc::clone(&cache);
        let waiting_thread = thread::spawn(move || {
            let mut buffer = [0u8; 1];
            waiting_cache.read_locked(64, &mut buffer).unwrap();
        });

        wait_until(Duration::from_secs(1), || {
            cache
                .state
                .lock()
                .unwrap()
                .foreground_dirty_eviction_waiters
                == 1
        });

        let (tx, rx) = mpsc::channel();
        let hit_cache = Arc::clone(&cache);
        let hit_thread = thread::spawn(move || {
            let mut buffer = [0u8; 4];
            hit_cache.read_locked(96, &mut buffer).unwrap();
            hit_cache.write_locked(100, &[250, 251]).unwrap();
            tx.send(buffer).unwrap();
        });

        assert_eq!(
            rx.recv_timeout(Duration::from_millis(200)).unwrap(),
            [96, 97, 98, 99]
        );
        hit_thread.join().unwrap();

        {
            let mut state = cache.state.lock().unwrap();
            let removed = state.spilled_dirty.remove(&0);
            assert!(removed.is_some());
        }
        cache.temp_store().remove_block(0).unwrap();
        cache.state_changed.notify_all();
        waiting_thread.join().unwrap();

        let mut verify = [0u8; 8];
        cache.read_locked(96, &mut verify).unwrap();
        assert_eq!(&verify[..], &[96, 97, 98, 99, 250, 251, 102, 103]);
    }

    #[test]
    fn worker_snapshot_creation_yields_to_foreground_temp_waiters() {
        let temp_dir = TestTempDir::new();
        let right = MockAtIo::with_len(192);
        let mut config = test_config(32);
        config.fifo_capacity_blocks = 1;
        config.lru_capacity_blocks = 1;
        config.dirty_scan_interval = Duration::from_secs(60);
        config.temp_dir = temp_dir.path().to_path_buf();
        let cache = Arc::new(Cache::new(config, right.clone()).unwrap());

        cache.read_locked(96, &mut [0u8; 1]).unwrap();
        cache.read_locked(96, &mut [0u8; 1]).unwrap();
        cache.write_locked(100, &[250, 251]).unwrap();
        cache.write_locked(4, &[200, 201, 202]).unwrap();
        cache.write_locked(36, &[210, 211]).unwrap();
        right.take_calls();

        let waiting_cache = Arc::clone(&cache);
        let waiting_thread = thread::spawn(move || {
            let mut buffer = [0u8; 1];
            waiting_cache.read_locked(64, &mut buffer).unwrap();
        });

        wait_until(Duration::from_secs(1), || {
            cache
                .state
                .lock()
                .unwrap()
                .foreground_dirty_eviction_waiters
                == 1
        });

        {
            let mut state = cache.state.lock().unwrap();
            let removed = state.spilled_dirty.remove(&0);
            assert!(removed.is_some());
            let created = Cache::<MockAtIo>::create_snapshot_temp_locked(
                cache.config(),
                &mut state,
                cache.temp_store(),
            )
            .unwrap();
            assert!(!created);
            assert!(
                state
                    .resident
                    .get(3)
                    .unwrap()
                    .state
                    .active_snapshot
                    .is_none()
            );
        }
        cache.temp_store().remove_block(0).unwrap();
        cache.state_changed.notify_all();
        waiting_thread.join().unwrap();

        let state = cache.state.lock().unwrap();
        assert!(
            state
                .resident
                .get(3)
                .unwrap()
                .state
                .active_snapshot
                .is_none()
        );
    }

    #[test]
    fn stop_requested_wakes_dirty_eviction_waiters() {
        let temp_dir = TestTempDir::new();
        let right = MockAtIo::with_len(160);
        let mut config = test_config(32);
        config.fifo_capacity_blocks = 1;
        config.dirty_scan_interval = Duration::from_secs(60);
        config.temp_dir = temp_dir.path().to_path_buf();
        let cache = Arc::new(Cache::new(config, right).unwrap());

        cache.write_locked(4, &[200, 201, 202]).unwrap();
        cache.write_locked(36, &[210, 211]).unwrap();

        let (tx, rx) = mpsc::channel();
        let waiting_cache = Arc::clone(&cache);
        let waiting_thread = thread::spawn(move || {
            let mut buffer = [0u8; 1];
            let result = waiting_cache.read_locked(64, &mut buffer);
            tx.send(result).unwrap();
        });

        wait_until(Duration::from_secs(1), || {
            cache
                .state
                .lock()
                .unwrap()
                .foreground_dirty_eviction_waiters
                == 1
        });

        {
            let mut state = cache.state.lock().unwrap();
            state.stop_requested = true;
        }
        cache.state_changed.notify_all();

        assert_eq!(
            rx.recv_timeout(Duration::from_secs(2)).unwrap(),
            Err(CacheError::Stopped)
        );
        waiting_thread.join().unwrap();
        assert_eq!(
            cache
                .state
                .lock()
                .unwrap()
                .foreground_dirty_eviction_waiters,
            0
        );
    }

    #[test]
    fn worker_flushes_resident_dirty_and_removes_temp() {
        let temp_dir = TestTempDir::new();
        let right = MockAtIo::with_len(128);
        let mut config = test_config(32);
        config.dirty_scan_interval = Duration::from_millis(20);
        config.temp_dir = temp_dir.path().to_path_buf();
        let cache = Cache::new(config, right.clone()).unwrap();
        let temp_path = cache.temp_store().path_for_block(0);

        cache.write_locked(4, &[200, 201, 202]).unwrap();

        wait_until(Duration::from_secs(2), || {
            let state = cache.state.lock().unwrap();
            let entry = match state.resident.get(0) {
                Some(entry) => entry,
                None => return false,
            };
            entry.state.active_snapshot.is_none()
                && entry.state.dirty_ranges.is_empty()
                && !temp_path.exists()
                && right.storage_slice(0, 8) == vec![0, 1, 2, 3, 200, 201, 202, 7]
        });

        let calls = right.take_calls();
        assert!(calls.contains(&IoCall::Read {
            offset: 0,
            length: 32,
        }));
        assert!(calls.contains(&IoCall::Write {
            offset: 0,
            length: 32,
        }));
    }

    #[test]
    fn worker_retries_failed_snapshot_flush_and_keeps_temp_until_success() {
        let temp_dir = TestTempDir::new();
        let right = MockAtIo::new(
            test_bytes(128),
            Duration::ZERO,
            Duration::from_millis(120),
            1,
        );
        let mut config = test_config(32);
        config.dirty_scan_interval = Duration::from_millis(80);
        config.temp_dir = temp_dir.path().to_path_buf();
        let cache = Cache::new(config, right.clone()).unwrap();
        let temp_path = cache.temp_store().path_for_block(0);

        cache.write_locked(4, &[200, 201, 202]).unwrap();

        wait_until(Duration::from_secs(2), || {
            let state = cache.state.lock().unwrap();
            let entry = match state.resident.get(0) {
                Some(entry) => entry,
                None => return false,
            };
            entry.state.active_snapshot.is_some()
                && entry.state.dirty_ranges.is_empty()
                && temp_path.is_file()
                && right.storage_slice(0, 8) == vec![0, 1, 2, 3, 4, 5, 6, 7]
        });

        wait_until(Duration::from_secs(2), || {
            let state = cache.state.lock().unwrap();
            let entry = match state.resident.get(0) {
                Some(entry) => entry,
                None => return false,
            };
            entry.state.active_snapshot.is_none()
                && entry.state.dirty_ranges.is_empty()
                && !temp_path.exists()
                && right.storage_slice(0, 8) == vec![0, 1, 2, 3, 200, 201, 202, 7]
        });

        let write_count = right
            .take_calls()
            .into_iter()
            .filter(|call| {
                matches!(
                    call,
                    IoCall::Write {
                        offset: 0,
                        length: 32
                    }
                )
            })
            .count();
        assert_eq!(write_count, 2);
    }

    #[test]
    fn worker_flushes_spilled_dirty_and_clears_spilled_entry() {
        let temp_dir = TestTempDir::new();
        let right = MockAtIo::with_len(128);
        let mut config = test_config(32);
        config.fifo_capacity_blocks = 1;
        config.dirty_scan_interval = Duration::from_millis(20);
        config.temp_dir = temp_dir.path().to_path_buf();
        let cache = Cache::new(config, right.clone()).unwrap();
        let temp_path = cache.temp_store().path_for_block(0);

        cache.write_locked(4, &[200, 201, 202]).unwrap();
        cache.read_locked(32, &mut [0u8; 1]).unwrap();

        wait_until(Duration::from_secs(2), || {
            let state = cache.state.lock().unwrap();
            state.spilled_dirty.is_empty()
                && !temp_path.exists()
                && right.storage_slice(0, 8) == vec![0, 1, 2, 3, 200, 201, 202, 7]
        });

        let mut buffer = [0u8; 8];
        cache.read_locked(0, &mut buffer).unwrap();

        assert_eq!(&buffer[..], &[0, 1, 2, 3, 200, 201, 202, 7]);
        assert!(right.take_calls().contains(&IoCall::Write {
            offset: 0,
            length: 32,
        }));
    }

    #[test]
    fn spilled_rehydrate_waits_for_worker_flush_then_reads_remote() {
        let temp_dir = TestTempDir::new();
        let right = MockAtIo::with_write_delay(128, Duration::from_millis(150));
        let mut config = test_config(32);
        config.fifo_capacity_blocks = 1;
        config.dirty_scan_interval = Duration::from_millis(20);
        config.temp_dir = temp_dir.path().to_path_buf();
        let cache = Arc::new(Cache::new(config, right.clone()).unwrap());

        cache.write_locked(4, &[200, 201, 202]).unwrap();
        cache.read_locked(32, &mut [0u8; 1]).unwrap();

        wait_until(Duration::from_secs(2), || {
            cache.state.lock().unwrap().active_temp_blocks.contains(&0)
        });

        let (tx, rx) = mpsc::channel();
        let read_cache = Arc::clone(&cache);
        let read_thread = thread::spawn(move || {
            let mut buffer = [0u8; 8];
            read_cache.read_locked(0, &mut buffer).unwrap();
            tx.send(buffer).unwrap();
        });

        assert!(rx.recv_timeout(Duration::from_millis(50)).is_err());
        let buffer = rx.recv_timeout(Duration::from_secs(2)).unwrap();
        read_thread.join().unwrap();

        assert_eq!(&buffer[..], &[0, 1, 2, 3, 200, 201, 202, 7]);

        let calls = right.take_calls();
        assert!(calls.contains(&IoCall::Write {
            offset: 0,
            length: 32,
        }));
        assert!(
            calls
                .iter()
                .filter(|call| {
                    matches!(
                        call,
                        IoCall::Read {
                            offset: 0,
                            length: 32
                        }
                    )
                })
                .count()
                >= 2
        );
    }
}
