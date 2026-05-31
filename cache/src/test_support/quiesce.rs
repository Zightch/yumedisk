use std::thread;
use std::time::{Duration, Instant};

use crate::{AtIo, Cache};

use super::{CacheSnapshot, LoadStateSnapshot, TestAtIo};

const POLL_INTERVAL: Duration = Duration::from_millis(10);

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct QuiesceTimeout {
    pub last_snapshot: CacheSnapshot,
    pub in_flight_right_io: usize,
}

pub fn wait_until<F>(timeout: Duration, mut predicate: F)
where
    F: FnMut() -> bool,
{
    let deadline = Instant::now() + timeout;
    while Instant::now() < deadline {
        if predicate() {
            return;
        }
        thread::sleep(POLL_INTERVAL);
    }

    assert!(predicate(), "condition not reached within {timeout:?}");
}

pub fn wait_for_quiesce<R>(
    cache: &Cache<R>,
    timeout: Duration,
) -> Result<CacheSnapshot, QuiesceTimeout>
where
    R: AtIo + TestAtIo + 'static,
{
    let deadline = Instant::now() + timeout;
    loop {
        let snapshot = cache.debug_snapshot();
        let in_flight_right_io = cache.right().in_flight_count();
        if snapshot.is_quiescent() && in_flight_right_io == 0 {
            return Ok(snapshot);
        }
        if Instant::now() >= deadline {
            return Err(QuiesceTimeout {
                last_snapshot: snapshot,
                in_flight_right_io,
            });
        }
        thread::sleep(POLL_INTERVAL);
    }
}

impl CacheSnapshot {
    pub fn is_quiescent(&self) -> bool {
        if self.stop_requested
            || !self.spilled_dirty.is_empty()
            || !self.active_temp_blocks.is_empty()
            || self.foreground_dirty_eviction_waiters != 0
        {
            return false;
        }

        self.resident.blocks.iter().all(|block| {
            block.load_state == LoadStateSnapshot::Ready
                && block.dirty_ranges.is_empty()
                && !block.has_active_snapshot
                && block.pending_patch_count == 0
        })
    }
}

#[cfg(test)]
mod tests {
    use std::time::Duration;

    use super::wait_for_quiesce;
    use crate::test_support::{MemoryAtIo, TestTempDir};
    use crate::{Cache, CacheConfig};

    fn test_bytes(length: usize) -> Vec<u8> {
        (0..length).map(|index| index as u8).collect()
    }

    #[test]
    fn wait_for_quiesce_returns_after_background_flush() {
        let temp_dir = TestTempDir::with_prefix("cache-quiesce");
        let right = MemoryAtIo::from_bytes(32, test_bytes(128));
        let config = CacheConfig {
            fifo_capacity_blocks: 2,
            lru_capacity_blocks: 2,
            block_size_bytes: 32,
            dirty_scan_interval: Duration::from_millis(20),
            temp_max_files: 2,
            temp_dir: temp_dir.path().to_path_buf(),
        };
        let cache = Cache::new(config, right).unwrap();

        cache.write_locked(4, &[200, 201, 202]).unwrap();

        let snapshot = wait_for_quiesce(&cache, Duration::from_secs(2)).unwrap();
        assert!(snapshot.is_quiescent());
        assert_eq!(
            cache.right().storage_slice(0, 8),
            vec![0, 1, 2, 3, 200, 201, 202, 7]
        );
        let log = cache.right().take_log();
        assert!(log.iter().any(|entry| {
            entry.offset == 0
                && entry.length == 32
                && matches!(entry.operation, crate::test_support::IoOperation::Read)
        }));
        assert!(log.iter().any(|entry| {
            entry.offset == 0
                && entry.length == 32
                && matches!(entry.operation, crate::test_support::IoOperation::Write)
        }));
    }
}
