use std::io::ErrorKind;
use std::time::Duration;

use crate::test_support::{
    DeterministicRng, IoFailureController, IoOperation, IoTimings, MemoryAtIo, ReferenceModel,
    TempFailureController, TempFaultOperation, TestHooks, TestTempDir, assert_quiesced_invariants,
    assert_runtime_invariants, assert_temp_artifacts_cleared,
};
use crate::{Cache, CacheConfig, CacheError};

const BLOCK_SIZE_BYTES: u32 = 48;
const BLOCK_SIZE: usize = BLOCK_SIZE_BYTES as usize;
const TOTAL_BLOCKS: usize = 6;
const TOTAL_BYTES: usize = BLOCK_SIZE * TOTAL_BLOCKS;
const QUIESCE_TIMEOUT: Duration = Duration::from_secs(5);

fn stage6_config(temp_dir: &TestTempDir) -> CacheConfig {
    CacheConfig {
        fifo_capacity_blocks: 1,
        lru_capacity_blocks: 1,
        block_size_bytes: BLOCK_SIZE_BYTES,
        dirty_scan_interval: Duration::from_millis(2),
        temp_max_files: 1,
        temp_dir: temp_dir.path().to_path_buf(),
    }
}

fn stage6_bytes(length: usize) -> Vec<u8> {
    (0..length).map(|index| index as u8).collect()
}

fn random_range(rng: &mut DeterministicRng, max_len: usize) -> (u64, usize) {
    let length = rng.range_usize(1, max_len + 1);
    let start = rng.next_usize(TOTAL_BYTES - length + 1);
    (start as u64, length)
}

fn random_single_block_range(rng: &mut DeterministicRng) -> (u64, usize, u64) {
    let block_index = rng.next_usize(TOTAL_BLOCKS) as u64;
    let block_base = block_index as usize * BLOCK_SIZE;
    let within_block = rng.next_usize(BLOCK_SIZE);
    let length = rng.range_usize(1, BLOCK_SIZE - within_block + 1);
    ((block_base + within_block) as u64, length, block_index)
}

fn assert_remote_matches_model(right: &MemoryAtIo, model: &ReferenceModel) {
    assert_eq!(right.storage_slice(0, model.len()), model.as_slice());
}

fn inject_fault_for_current_block(
    step: usize,
    is_write: bool,
    block_index: u64,
    io_failures: &IoFailureController,
    temp_failures: &TempFailureController,
) {
    if is_write {
        match step % 5 {
            0 => {
                io_failures.fail_once(
                    IoOperation::Read,
                    Some(block_index),
                    CacheError::NotImplemented,
                );
            }
            1 => {
                io_failures.fail_once(
                    IoOperation::Write,
                    Some(block_index),
                    CacheError::NotImplemented,
                );
            }
            2 => {
                temp_failures.fail_once(
                    TempFaultOperation::Write,
                    Some(block_index),
                    ErrorKind::PermissionDenied,
                );
            }
            3 => {
                temp_failures.fail_once(
                    TempFaultOperation::Read,
                    Some(block_index),
                    ErrorKind::Interrupted,
                );
            }
            _ => {
                temp_failures.fail_once(
                    TempFaultOperation::Delete,
                    Some(block_index),
                    ErrorKind::PermissionDenied,
                );
            }
        }
    } else {
        match step % 4 {
            0 => {
                io_failures.fail_once(
                    IoOperation::Read,
                    Some(block_index),
                    CacheError::NotImplemented,
                );
            }
            1 => {
                temp_failures.fail_once(
                    TempFaultOperation::Read,
                    Some(block_index),
                    ErrorKind::Interrupted,
                );
            }
            2 => {
                temp_failures.fail_once(
                    TempFaultOperation::Write,
                    Some(block_index),
                    ErrorKind::PermissionDenied,
                );
            }
            _ => {
                temp_failures.fail_once(
                    TempFaultOperation::Delete,
                    Some(block_index),
                    ErrorKind::PermissionDenied,
                );
            }
        }
    }
}

#[test]
fn randomized_small_capacity_pressure_matches_reference_model() {
    let temp_dir = TestTempDir::with_prefix("cache-p2-pressure");
    let right = MemoryAtIo::with_timings(
        BLOCK_SIZE_BYTES,
        stage6_bytes(TOTAL_BYTES),
        IoTimings {
            read_delay: Duration::from_millis(1),
            write_delay: Duration::from_millis(3),
        },
    );
    let config = stage6_config(&temp_dir);
    let cache = Cache::new(config.clone(), right.clone()).unwrap();
    let mut model = ReferenceModel::from_bytes(stage6_bytes(TOTAL_BYTES));
    let mut rng = DeterministicRng::new(0xA11C_E501);

    for step in 0..180 {
        if !rng.one_in(3) {
            let (offset, length) = random_range(&mut rng, BLOCK_SIZE * 2);
            let mut data = vec![0u8; length];
            rng.fill_bytes(&mut data);
            cache.write_locked(offset, &data).unwrap();
            model.write(offset, &data);
        } else {
            let (offset, length) = random_range(&mut rng, BLOCK_SIZE * 2);
            let mut buffer = vec![0u8; length];
            cache.read_locked(offset, &mut buffer).unwrap();
            assert_eq!(buffer, model.read(offset, length));
        }

        if step % 8 == 7 {
            assert_runtime_invariants(&cache, &config);
        }
        if step % 45 == 44 {
            assert_quiesced_invariants(&cache, &config, QUIESCE_TIMEOUT);
            assert_remote_matches_model(&right, &model);
            assert_temp_artifacts_cleared(temp_dir.path());
        }
    }

    assert_quiesced_invariants(&cache, &config, QUIESCE_TIMEOUT);
    assert_remote_matches_model(&right, &model);
    assert_temp_artifacts_cleared(temp_dir.path());

    let log = right.log_snapshot();
    assert!(log.iter().any(|entry| entry.operation == IoOperation::Read));
    assert!(
        log.iter()
            .any(|entry| entry.operation == IoOperation::Write)
    );
}

#[test]
fn randomized_fault_regression_recovers_to_reference_model() {
    let temp_dir = TestTempDir::with_prefix("cache-p2-faults");
    let io_failures = IoFailureController::new();
    let temp_failures = TempFailureController::new();
    let right = MemoryAtIo::with_failures(
        BLOCK_SIZE_BYTES,
        stage6_bytes(TOTAL_BYTES),
        IoTimings {
            read_delay: Duration::ZERO,
            write_delay: Duration::from_millis(2),
        },
        io_failures.clone(),
    );
    let config = stage6_config(&temp_dir);
    let cache = Cache::new_for_test_with_temp_failures(
        config.clone(),
        right.clone(),
        TestHooks::default(),
        temp_failures.clone(),
    )
    .unwrap();
    let mut model = ReferenceModel::from_bytes(stage6_bytes(TOTAL_BYTES));
    let mut rng = DeterministicRng::new(0xBADC_0FFE);
    let mut operation_error_count = 0usize;

    for step in 0..160 {
        let is_write = !rng.one_in(3);
        let (offset, length, block_index) = random_single_block_range(&mut rng);

        if step % 3 == 1 {
            inject_fault_for_current_block(
                step,
                is_write,
                block_index,
                &io_failures,
                &temp_failures,
            );
        }

        if is_write {
            let mut data = vec![0u8; length];
            rng.fill_bytes(&mut data);
            match cache.write_locked(offset, &data) {
                Ok(()) => model.write(offset, &data),
                Err(_) => operation_error_count += 1,
            }
        } else {
            let mut buffer = vec![0u8; length];
            match cache.read_locked(offset, &mut buffer) {
                Ok(()) => assert_eq!(buffer, model.read(offset, length)),
                Err(_) => operation_error_count += 1,
            }
        }

        if step % 6 == 5 {
            assert_runtime_invariants(&cache, &config);
        }
        if step % 32 == 31 {
            io_failures.clear_all();
            temp_failures.clear_all();
            assert_quiesced_invariants(&cache, &config, QUIESCE_TIMEOUT);
            assert_remote_matches_model(&right, &model);
            assert_temp_artifacts_cleared(temp_dir.path());
        }
    }

    io_failures.clear_all();
    temp_failures.clear_all();
    assert_quiesced_invariants(&cache, &config, QUIESCE_TIMEOUT);
    assert_remote_matches_model(&right, &model);
    assert_temp_artifacts_cleared(temp_dir.path());

    assert!(operation_error_count > 0);
    assert!(
        right
            .log_snapshot()
            .iter()
            .any(|entry| entry.result.is_err())
    );
}
