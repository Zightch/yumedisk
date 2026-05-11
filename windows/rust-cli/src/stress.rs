use std::thread;
use std::time::Duration;
use std::time::Instant;

use backend_rust::BackendContext;
use backend_rust::DiskConfig;
use backend_rust::ManagedDiskSnapshot;
use backend_rust::YUMEDISK_MAX_TARGETS;

use crate::dense_mem::DenseMem;
use crate::disk_io::DeviceHandle;

#[derive(Debug, Clone)]
pub struct SmokeConfig {
    pub disk_mib: u64,
    pub threads: usize,
    pub iterations_per_thread: usize,
    pub block_kib: usize,
    pub target_id: Option<u32>,
}

impl Default for SmokeConfig {
    fn default() -> Self {
        Self {
            disk_mib: 64,
            threads: 4,
            iterations_per_thread: 32,
            block_kib: 4,
            target_id: None,
        }
    }
}

pub fn run_smoke(config: SmokeConfig) -> Result<(), String> {
    let context = BackendContext::new();
    println!("state=startup");

    if !context.open() {
        let error = context
            .snapshot_log_lines()
            .last()
            .cloned()
            .unwrap_or_else(|| "open-failed".to_string());
        return Err(error);
    }
    println!("state=open");
    println!("session={}", context.query_session_state_text());

    let disk_size_bytes = config
        .disk_mib
        .checked_mul(1024 * 1024)
        .ok_or_else(|| "disk-size-overflow".to_string())?;
    let target_id = config
        .target_id
        .unwrap_or_else(|| context.find_first_free_target());
    if target_id >= YUMEDISK_MAX_TARGETS {
        return Err("no-free-target".to_string());
    }

    let dense_mem = DenseMem::new(disk_size_bytes).map_err(|error| error.to_string())?;
    let mut disk_config = DiskConfig::default();
    disk_config.target_id = target_id;
    disk_config.disk_size_bytes = disk_size_bytes;

    let mut create_error = String::new();
    if !context.create_managed_disk(
        disk_config,
        Box::new(dense_mem.clone()),
        Some(&mut create_error),
    ) {
        if create_error.is_empty() {
            create_error = "create-failed".to_string();
        }
        context.close();
        return Err(create_error);
    }
    println!("state=create target={}", target_id);

    let snapshot = wait_for_disk_ready(&context, target_id, Duration::from_secs(5))?;
    println!(
        "disk=ready target={} visible_path={} physical_drive={}",
        snapshot.target_id, snapshot.visible_path, snapshot.physical_drive_path
    );

    run_concurrent_read_write_test(
        &snapshot.physical_drive_path,
        disk_size_bytes,
        config.threads,
        config.iterations_per_thread,
        config.block_kib * 1024,
    )?;
    println!("state=rw-test ok");

    println!("state=verify-begin");
    wait_for_dense_memory_verify(
        &dense_mem,
        disk_size_bytes,
        config.threads,
        config.iterations_per_thread,
        config.block_kib * 1024,
        Duration::from_secs(5),
    )?;
    println!("state=memory-verify ok");

    println!("state=remove-begin target={}", target_id);
    let mut remove_error = String::new();
    if !context.remove_managed_disk(target_id, Some(&mut remove_error)) {
        if remove_error.is_empty() {
            remove_error = "remove-failed".to_string();
        }
        context.close();
        return Err(remove_error);
    }
    println!("state=remove target={}", target_id);

    context.close();
    println!("state=exit ok");
    Ok(())
}

fn wait_for_disk_ready(
    context: &BackendContext,
    target_id: u32,
    timeout: Duration,
) -> Result<ManagedDiskSnapshot, String> {
    let deadline = Instant::now()
        .checked_add(timeout)
        .ok_or_else(|| "timeout-overflow".to_string())?;

    loop {
        for disk in context.snapshot_managed_disks() {
            if disk.target_id == target_id && !disk.physical_drive_path.is_empty() {
                return Ok(disk);
            }
        }

        if Instant::now() >= deadline {
            return Err(format!("disk-ready-timeout target={}", target_id));
        }

        thread::sleep(Duration::from_millis(100));
    }
}

fn run_concurrent_read_write_test(
    physical_drive_path: &str,
    disk_size_bytes: u64,
    threads: usize,
    iterations_per_thread: usize,
    block_bytes: usize,
) -> Result<(), String> {
    if threads == 0 {
        return Err("thread-count-zero".to_string());
    }
    if block_bytes == 0 {
        return Err("block-size-zero".to_string());
    }
    if disk_size_bytes < (threads as u64 * block_bytes as u64) {
        return Err("disk-too-small-for-test".to_string());
    }

    let region_bytes = (disk_size_bytes / threads as u64) as usize;
    let ops_per_thread = region_bytes / block_bytes;
    let effective_iterations = iterations_per_thread.min(ops_per_thread);
    if effective_iterations == 0 {
        return Err("no-usable-iterations".to_string());
    }

    let mut workers = Vec::with_capacity(threads);
    for thread_index in 0..threads {
        let path = physical_drive_path.to_string();
        workers.push(thread::spawn(move || {
            let handle = DeviceHandle::open(&path, true)?;
            for iteration in 0..effective_iterations {
                let offset = thread_offset(thread_index, iteration, region_bytes, block_bytes)?;
                let expected = build_pattern(thread_index, iteration, offset, block_bytes);
                handle.write_all_at(offset, &expected)?;
                let mut actual = vec![0u8; block_bytes];
                handle.read_exact_at(offset, &mut actual)?;
                if actual != expected {
                    return Err(format!(
                        "rw-verify-mismatch thread={} iteration={} offset={}",
                        thread_index, iteration, offset
                    ));
                }
            }
            Ok::<(), String>(())
        }));
    }

    for worker in workers {
        worker.join().map_err(|_| "worker-panicked".to_string())??;
    }

    Ok(())
}

fn wait_for_dense_memory_verify(
    dense_mem: &DenseMem,
    disk_size_bytes: u64,
    threads: usize,
    iterations_per_thread: usize,
    block_bytes: usize,
    timeout: Duration,
) -> Result<(), String> {
    let deadline = Instant::now()
        .checked_add(timeout)
        .ok_or_else(|| "timeout-overflow".to_string())?;

    loop {
        match verify_dense_memory_once(
            dense_mem,
            disk_size_bytes,
            threads,
            iterations_per_thread,
            block_bytes,
        ) {
            Ok(()) => return Ok(()),
            Err(error) => {
                if Instant::now() >= deadline {
                    return Err(error);
                }
                thread::sleep(Duration::from_millis(50));
            }
        }
    }
}

fn verify_dense_memory_once(
    dense_mem: &DenseMem,
    disk_size_bytes: u64,
    threads: usize,
    iterations_per_thread: usize,
    block_bytes: usize,
) -> Result<(), String> {
    let region_bytes = (disk_size_bytes / threads as u64) as usize;
    let ops_per_thread = region_bytes / block_bytes;
    let effective_iterations = iterations_per_thread.min(ops_per_thread);

    for thread_index in 0..threads {
        for iteration in 0..effective_iterations {
            let offset = thread_offset(thread_index, iteration, region_bytes, block_bytes)?;
            let expected = build_pattern(thread_index, iteration, offset, block_bytes);
            let actual = dense_mem
                .snapshot_range(offset, block_bytes)
                .map_err(|error| error.to_string())?;
            if actual != expected {
                return Err(format!(
                    "dense-mem-mismatch thread={} iteration={} offset={}",
                    thread_index, iteration, offset
                ));
            }
        }
    }

    Ok(())
}

fn thread_offset(
    thread_index: usize,
    iteration: usize,
    region_bytes: usize,
    block_bytes: usize,
) -> Result<u64, String> {
    let region_begin = thread_index
        .checked_mul(region_bytes)
        .ok_or_else(|| "offset-overflow".to_string())?;
    let iteration_offset = iteration
        .checked_mul(block_bytes)
        .ok_or_else(|| "offset-overflow".to_string())?;
    let offset = region_begin
        .checked_add(iteration_offset)
        .ok_or_else(|| "offset-overflow".to_string())?;
    u64::try_from(offset).map_err(|_| "offset-overflow".to_string())
}

fn build_pattern(
    thread_index: usize,
    iteration: usize,
    offset: u64,
    block_bytes: usize,
) -> Vec<u8> {
    let mut buffer = vec![0u8; block_bytes];
    if buffer.len() >= 8 {
        buffer[0..8].copy_from_slice(&(thread_index as u64).to_le_bytes());
    }
    if buffer.len() >= 16 {
        buffer[8..16].copy_from_slice(&(iteration as u64).to_le_bytes());
    }
    if buffer.len() >= 24 {
        buffer[16..24].copy_from_slice(&offset.to_le_bytes());
    }
    for index in 24..buffer.len() {
        let lane = (index % 8) as u32;
        let rotated = offset.rotate_left(lane);
        let mix = rotated as u8
            ^ (thread_index as u8).wrapping_mul(31)
            ^ (iteration as u8).wrapping_mul(17)
            ^ (index as u8).wrapping_mul(13);
        buffer[index] = mix;
    }
    buffer
}
