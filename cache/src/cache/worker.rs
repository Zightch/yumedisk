use std::sync::{Arc, Condvar, Mutex};
use std::thread::{self, JoinHandle};
use std::time::Duration;

use super::Cache;
use super::state::{CacheState, WorkerAction};
use crate::block::BlockLayout;
use crate::resident::LoadState;
use crate::temp::TempStore;
#[cfg(any(test, feature = "test-hooks"))]
use crate::test_support::{HookPoint, TestHooks};
use crate::{AtIo, CacheConfig, CacheError};

impl<R: AtIo + 'static> Cache<R> {
    pub(super) fn spawn_flush_worker(
        config: CacheConfig,
        layout: BlockLayout,
        state: Arc<Mutex<CacheState>>,
        state_changed: Arc<Condvar>,
        temp: TempStore,
        right: Arc<R>,
        #[cfg(any(test, feature = "test-hooks"))] hooks: TestHooks,
    ) -> JoinHandle<()> {
        thread::Builder::new()
            .name("cache-flush-worker".into())
            .spawn(move || {
                Self::run_flush_worker(
                    config,
                    layout,
                    state,
                    state_changed,
                    temp,
                    right,
                    #[cfg(any(test, feature = "test-hooks"))]
                    hooks,
                );
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
        #[cfg(any(test, feature = "test-hooks"))] hooks: TestHooks,
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
                        #[cfg(any(test, feature = "test-hooks"))]
                        &hooks,
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

    pub(super) fn create_snapshot_temp_locked(
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
        #[cfg(any(test, feature = "test-hooks"))] hooks: &TestHooks,
    ) -> Result<(), CacheError> {
        let mut temp_buffer = vec![0u8; layout.block_size_usize()?];
        let flush_result = temp
            .read_block(block_index, &mut temp_buffer)
            .and_then(|_| {
                layout.validate_right_io(block_base, temp_buffer.len())?;
                #[cfg(any(test, feature = "test-hooks"))]
                hooks.reach_gate(HookPoint::BeforeRightWrite);
                let result = right.write_at(block_base, &temp_buffer);
                #[cfg(any(test, feature = "test-hooks"))]
                hooks.reach_gate(HookPoint::AfterRightWrite);
                result
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
}
