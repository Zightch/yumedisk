use std::collections::{HashMap, HashSet};
use std::fs;
use std::path::{Path, PathBuf};
use std::time::Duration;

use crate::{AtIo, Cache, CacheConfig};

use super::{
    CacheSnapshot, IoLogEntry, LoadStateSnapshot, QueueKindSnapshot, TestAtIo, wait_for_quiesce,
};

const DEFAULT_NONZERO_SEED: u64 = 0x4D59_5DF4_D0F3_3173;

#[derive(Debug, Clone)]
pub struct DeterministicRng {
    state: u64,
}

impl DeterministicRng {
    pub fn new(seed: u64) -> Self {
        Self {
            state: if seed == 0 {
                DEFAULT_NONZERO_SEED
            } else {
                seed
            },
        }
    }

    pub fn next_u64(&mut self) -> u64 {
        let mut state = self.state;
        state ^= state << 13;
        state ^= state >> 7;
        state ^= state << 17;
        if state == 0 {
            state = DEFAULT_NONZERO_SEED;
        }
        self.state = state;
        state
    }

    pub fn next_bool(&mut self) -> bool {
        self.next_u64() & 1 == 0
    }

    pub fn one_in(&mut self, denominator: u64) -> bool {
        assert!(denominator > 0, "denominator must be greater than 0");
        self.next_u64() % denominator == 0
    }

    pub fn next_usize(&mut self, upper_exclusive: usize) -> usize {
        assert!(
            upper_exclusive > 0,
            "upper_exclusive must be greater than 0"
        );
        (self.next_u64() % upper_exclusive as u64) as usize
    }

    pub fn range_usize(&mut self, start_inclusive: usize, end_exclusive: usize) -> usize {
        assert!(
            start_inclusive < end_exclusive,
            "start_inclusive must be smaller than end_exclusive"
        );
        start_inclusive + self.next_usize(end_exclusive - start_inclusive)
    }

    pub fn fill_bytes(&mut self, buffer: &mut [u8]) {
        for byte in buffer {
            *byte = self.next_u64() as u8;
        }
    }
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct ReferenceModel {
    bytes: Vec<u8>,
}

impl ReferenceModel {
    pub fn from_bytes(bytes: Vec<u8>) -> Self {
        Self { bytes }
    }

    pub fn len(&self) -> usize {
        self.bytes.len()
    }

    pub fn read(&self, offset: u64, length: usize) -> Vec<u8> {
        let range = checked_range(self.bytes.len(), offset, length);
        self.bytes[range].to_vec()
    }

    pub fn write(&mut self, offset: u64, data: &[u8]) {
        let range = checked_range(self.bytes.len(), offset, data.len());
        self.bytes[range].copy_from_slice(data);
    }

    pub fn as_slice(&self) -> &[u8] {
        &self.bytes
    }
}

pub fn assert_runtime_invariants<R>(cache: &Cache<R>, config: &CacheConfig) -> CacheSnapshot
where
    R: AtIo + TestAtIo + 'static,
{
    let snapshot = cache.debug_snapshot();
    assert_snapshot_invariants(&snapshot, config);
    assert_right_io_invariants(&cache.right().log_snapshot(), config);
    snapshot
}

pub fn assert_quiesced_invariants<R>(
    cache: &Cache<R>,
    config: &CacheConfig,
    timeout: Duration,
) -> CacheSnapshot
where
    R: AtIo + TestAtIo + 'static,
{
    let snapshot = wait_for_quiesce(cache, timeout).unwrap_or_else(|timeout_error| {
        panic!(
            "cache did not quiesce within {timeout:?}: {:?}",
            timeout_error
        )
    });
    assert!(snapshot.is_quiescent(), "quiesce snapshot must be stable");
    assert_snapshot_invariants(&snapshot, config);
    assert_right_io_invariants(&cache.right().log_snapshot(), config);
    snapshot
}

pub fn assert_snapshot_invariants(snapshot: &CacheSnapshot, config: &CacheConfig) {
    let block_size = config.block_size_bytes as usize;
    let resident_limit = config.fifo_capacity_blocks + config.lru_capacity_blocks;

    assert!(
        snapshot.resident.blocks.len() <= resident_limit,
        "resident block count {} exceeds limit {}",
        snapshot.resident.blocks.len(),
        resident_limit
    );
    assert!(
        snapshot.resident.fifo_order.len() <= config.fifo_capacity_blocks,
        "fifo length {} exceeds capacity {}",
        snapshot.resident.fifo_order.len(),
        config.fifo_capacity_blocks
    );
    assert!(
        snapshot.resident.lru_order.len() <= config.lru_capacity_blocks,
        "lru length {} exceeds capacity {}",
        snapshot.resident.lru_order.len(),
        config.lru_capacity_blocks
    );
    assert_eq!(
        snapshot.resident.blocks.len(),
        snapshot.resident.fifo_order.len() + snapshot.resident.lru_order.len(),
        "resident block list and queue orders diverged"
    );

    let fifo_set = unique_set("fifo_order", &snapshot.resident.fifo_order);
    let lru_set = unique_set("lru_order", &snapshot.resident.lru_order);
    assert!(
        fifo_set.is_disjoint(&lru_set),
        "fifo and lru order must not overlap: fifo={:?}, lru={:?}",
        snapshot.resident.fifo_order,
        snapshot.resident.lru_order
    );

    let mut resident_map = HashMap::new();
    for block in &snapshot.resident.blocks {
        assert!(
            resident_map.insert(block.block_index, block).is_none(),
            "duplicate resident block snapshot for {}",
            block.block_index
        );
        assert!(
            block.valid_len > 0 && block.valid_len <= block_size,
            "resident block {} has invalid valid_len {} for block size {}",
            block.block_index,
            block.valid_len,
            block_size
        );
        for range in &block.dirty_ranges {
            assert!(
                range.start < range.end,
                "resident block {} has empty dirty range {:?}",
                block.block_index,
                range
            );
            assert!(
                range.end <= block.valid_len,
                "resident block {} dirty range {:?} exceeds valid_len {}",
                block.block_index,
                range,
                block.valid_len
            );
        }

        match block.queue {
            QueueKindSnapshot::Fifo => {
                assert!(
                    fifo_set.contains(&block.block_index),
                    "resident fifo block {} missing from fifo order {:?}",
                    block.block_index,
                    snapshot.resident.fifo_order
                );
                assert!(
                    !lru_set.contains(&block.block_index),
                    "resident fifo block {} must not appear in lru order {:?}",
                    block.block_index,
                    snapshot.resident.lru_order
                );
            }
            QueueKindSnapshot::Lru => {
                assert!(
                    lru_set.contains(&block.block_index),
                    "resident lru block {} missing from lru order {:?}",
                    block.block_index,
                    snapshot.resident.lru_order
                );
                assert!(
                    !fifo_set.contains(&block.block_index),
                    "resident lru block {} must not appear in fifo order {:?}",
                    block.block_index,
                    snapshot.resident.fifo_order
                );
            }
        }

        match block.load_state {
            LoadStateSnapshot::Ready => {
                assert_eq!(
                    block.pending_patch_count, 0,
                    "ready resident block {} must not keep pending patches",
                    block.block_index
                );
            }
            LoadStateSnapshot::LoadingRemote | LoadStateSnapshot::Rehydrating => {
                assert!(
                    block.dirty_ranges.is_empty(),
                    "loading resident block {} must not expose dirty ranges {:?}",
                    block.block_index,
                    block.dirty_ranges
                );
                assert!(
                    !block.has_active_snapshot,
                    "loading resident block {} must not have active snapshot",
                    block.block_index
                );
            }
        }
    }

    let spilled_set = snapshot
        .spilled_dirty
        .iter()
        .map(|block| {
            assert!(
                block.valid_len > 0 && block.valid_len <= block_size,
                "spilled block {} has invalid valid_len {} for block size {}",
                block.block_index,
                block.valid_len,
                block_size
            );
            block.block_index
        })
        .collect::<Vec<_>>();
    let spilled_set = unique_set("spilled_dirty", &spilled_set);

    let active_snapshot_count = snapshot
        .resident
        .blocks
        .iter()
        .filter(|block| block.has_active_snapshot)
        .count();
    let logical_temp_file_count = spilled_set.len() + active_snapshot_count;
    assert!(
        logical_temp_file_count <= config.temp_max_files,
        "logical temp file count {} exceeds limit {}",
        logical_temp_file_count,
        config.temp_max_files
    );

    let active_temp_set = unique_set("active_temp_blocks", &snapshot.active_temp_blocks);
    assert!(
        active_temp_set.len() <= logical_temp_file_count,
        "active temp users {} exceed logical temp file count {}",
        active_temp_set.len(),
        logical_temp_file_count
    );
    for block_index in &snapshot.active_temp_blocks {
        let resident_uses_temp = resident_map.get(block_index).is_some_and(|block| {
            block.has_active_snapshot || block.load_state == LoadStateSnapshot::Rehydrating
        });
        assert!(
            spilled_set.contains(block_index) || resident_uses_temp,
            "active temp block {} is not backed by spilled or resident temp state",
            block_index
        );
    }

    if snapshot.foreground_dirty_eviction_waiters > 0 {
        assert!(
            logical_temp_file_count >= config.temp_max_files,
            "foreground waiters require temp pressure: waiters={}, temp_files={}, temp_max={}",
            snapshot.foreground_dirty_eviction_waiters,
            logical_temp_file_count,
            config.temp_max_files
        );
    }
}

pub fn collect_temp_artifacts(root: &Path) -> Vec<PathBuf> {
    let mut artifacts = Vec::new();
    if !root.exists() {
        return artifacts;
    }

    let entries = fs::read_dir(root).unwrap_or_else(|error| {
        panic!("failed to read temp dir {}: {error}", root.display());
    });
    for entry in entries {
        let path = entry.unwrap().path();
        if path.is_file() {
            artifacts.push(path);
        }
    }
    artifacts.sort();
    artifacts
}

pub fn assert_temp_artifacts_cleared(root: &Path) {
    let artifacts = collect_temp_artifacts(root);
    assert!(
        artifacts.is_empty(),
        "temp directory still contains artifacts: {:?}",
        artifacts
    );
}

fn assert_right_io_invariants(entries: &[IoLogEntry], config: &CacheConfig) {
    let block_size = config.block_size_bytes as usize;
    let block_size_u64 = u64::from(config.block_size_bytes);
    for entry in entries {
        assert_eq!(
            entry.offset % block_size_u64,
            0,
            "right io offset must stay block aligned: {:?}",
            entry
        );
        assert_eq!(
            entry.length, block_size,
            "right io length must stay fixed-size: {:?}",
            entry
        );
        assert_eq!(
            entry.block_index,
            entry.offset / block_size_u64,
            "right io block index must match offset: {:?}",
            entry
        );
    }
}

fn checked_range(total_len: usize, offset: u64, length: usize) -> std::ops::Range<usize> {
    let start = usize::try_from(offset).expect("reference model offset must fit usize");
    let end = start
        .checked_add(length)
        .expect("reference model range end must not overflow");
    assert!(
        end <= total_len,
        "reference model range {}..{} exceeds length {}",
        start,
        end,
        total_len
    );
    start..end
}

fn unique_set(label: &str, values: &[u64]) -> HashSet<u64> {
    let mut set = HashSet::new();
    for &value in values {
        assert!(
            set.insert(value),
            "{label} contains duplicate block {value}"
        );
    }
    set
}
