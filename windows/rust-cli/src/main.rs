mod dense_mem;
mod disk_io;
mod stress;

use std::collections::BTreeMap;
use std::env;
use std::io;
use std::io::Write;
use std::process;
use std::thread;
use std::time::Duration;
use std::time::Instant;

use backend_rust::BackendContext;
use backend_rust::BackendStatsSnapshot;
use backend_rust::DiskConfig;
use backend_rust::ManagedDiskSnapshot;
use backend_rust::YUMEDISK_MAX_TARGETS;
use backend_rust::YUMEDISK_MAX_USABLE_TARGET_ID;
use backend_rust::enumerate_visible_yumedisks;
use backend_rust::make_physical_drive_path;
use dense_mem::DenseMem;
use disk_io::DeviceHandle;
use stress::SmokeConfig;
use stress::run_smoke;

type AppResult<T> = Result<T, String>;

const DISK_READY_TIMEOUT: Duration = Duration::from_secs(5);
const MEMORY_COMMIT_TIMEOUT: Duration = Duration::from_secs(2);
const MEMORY_COMMIT_POLL: Duration = Duration::from_millis(50);

#[derive(Debug, Clone)]
struct CliDisk {
    dense_mem: DenseMem,
    disk_size_bytes: u64,
    read_only: bool,
}

#[derive(Debug, Clone, Copy)]
struct CreateDiskRequest {
    disk_size_mib: u64,
    read_only: bool,
    target_id: Option<u32>,
}

enum LoopControl {
    Continue,
    Exit,
}

struct CliHost {
    context: BackendContext,
    disks: BTreeMap<u32, CliDisk>,
}

struct AlignedRange {
    aligned_offset: u64,
    aligned_length: usize,
    inner_offset: usize,
}

fn main() {
    if let Err(error) = run() {
        eprintln!("error: {}", error);
        process::exit(1);
    }
}

fn run() -> AppResult<()> {
    let mut args = env::args().skip(1);
    match args.next().as_deref() {
        None | Some("shell") => run_shell(),
        Some("smoke") => {
            let config = parse_smoke_args(args)?;
            run_smoke(config)
        }
        Some("help") | Some("--help") | Some("-h") => {
            print_usage();
            Ok(())
        }
        Some(command) => Err(format!("unknown command: {}", command)),
    }
}

fn run_shell() -> AppResult<()> {
    let mut host = CliHost::open()?;
    println!("state=ready(rust-cli)");
    println!("{}", host.context.query_session_state_text());
    print_runtime_help();

    let loop_result = host.run_command_loop();
    host.shutdown();
    loop_result
}

impl CliHost {
    fn open() -> AppResult<Self> {
        let context = BackendContext::new();
        if !context.open() {
            let error = context
                .snapshot_log_lines()
                .last()
                .cloned()
                .unwrap_or_else(|| "open-failed".to_string());
            return Err(error);
        }

        Ok(Self {
            context,
            disks: BTreeMap::new(),
        })
    }

    fn shutdown(&mut self) {
        self.context.close();
        self.disks.clear();
    }

    fn run_command_loop(&mut self) -> AppResult<()> {
        let stdin = io::stdin();

        loop {
            print!("> ");
            io::stdout()
                .flush()
                .map_err(|error| format!("stdout-flush-failed: {}", error))?;

            let mut line = String::new();
            let read = stdin
                .read_line(&mut line)
                .map_err(|error| format!("stdin-read-failed: {}", error))?;
            if read == 0 {
                println!("state=eof");
                return Ok(());
            }

            let tokens: Vec<&str> = line.split_whitespace().collect();
            if tokens.is_empty() {
                continue;
            }

            match self.execute_command(tokens[0], &tokens[1..]) {
                Ok(LoopControl::Continue) => {}
                Ok(LoopControl::Exit) => return Ok(()),
                Err(error) => eprintln!("error: {}", error),
            }
        }
    }

    fn execute_command(&mut self, command: &str, args: &[&str]) -> AppResult<LoopControl> {
        match command.to_ascii_lowercase().as_str() {
            "help" => {
                print_runtime_help();
                Ok(LoopControl::Continue)
            }
            "state" | "query" => {
                self.print_state();
                Ok(LoopControl::Continue)
            }
            "stats" => {
                self.print_stats()?;
                Ok(LoopControl::Continue)
            }
            "ls" => {
                self.print_managed_disks();
                Ok(LoopControl::Continue)
            }
            "logs" => {
                self.print_logs(args)?;
                Ok(LoopControl::Continue)
            }
            "ct" => {
                self.create_disk(args)?;
                Ok(LoopControl::Continue)
            }
            "rm" => {
                self.remove_disk(args)?;
                Ok(LoopControl::Continue)
            }
            "rd" | "read" => {
                self.read_disk_bytes(args)?;
                Ok(LoopControl::Continue)
            }
            "wr" | "write" => {
                self.write_disk_bytes(args)?;
                Ok(LoopControl::Continue)
            }
            "peek" => {
                self.peek_memory_bytes(args)?;
                Ok(LoopControl::Continue)
            }
            "exit" | "quit" => Ok(LoopControl::Exit),
            other => Err(format!("unknown command: {}", other)),
        }
    }

    fn print_state(&self) {
        println!("{}", self.context.query_session_state_text());
    }

    fn print_stats(&self) -> AppResult<()> {
        let mut stats = BackendStatsSnapshot::default();
        let mut error_text = String::new();
        if !self
            .context
            .query_backend_stats(&mut stats, Some(&mut error_text))
        {
            if error_text.is_empty() {
                error_text = "stats-query-failed".to_string();
            }
            return Err(error_text);
        }

        println!(
            "heartbeat_sent={}, command_failures={}, protocol_failures={}, events_queued={}, events_dropped={}, disk_count={}",
            stats.heartbeat_sent,
            stats.command_failures,
            stats.protocol_failures,
            stats.events_queued,
            stats.events_dropped,
            stats.disk_count
        );
        Ok(())
    }

    fn print_managed_disks(&self) {
        let managed_disks = self.context.snapshot_managed_disks();
        println!("managed_target_count={}", managed_disks.len());
        for disk in managed_disks {
            println!(
                "target={}, disk_bytes={}, sector_size={}, read_only={}, backing=denseMem, lifecycle={}, online={}, visible_path={}, physical_drive={}",
                disk.target_id,
                disk.disk_size_bytes,
                disk.sector_size,
                bool_to_text(disk.read_only),
                disk.lifecycle_text,
                bool_to_text(disk.online),
                pending_text(&disk.visible_path),
                pending_text(&disk.physical_drive_path)
            );
        }

        let visible_disks = enumerate_visible_yumedisks();
        println!("visible_disk_count={}", visible_disks.len());
        for (index, disk) in visible_disks.iter().enumerate() {
            println!(
                "visible_disk[{}], path={}, device_number={}, disk_bytes={}, physical_drive={}",
                index,
                disk.path,
                disk.device_number,
                disk.length_bytes,
                make_physical_drive_path(disk.device_number)
            );
        }
    }

    fn print_logs(&self, args: &[&str]) -> AppResult<()> {
        let limit = if args.is_empty() {
            20usize
        } else {
            parse_usize_value(args[0], "log count")?
        };

        let log_lines = self.context.snapshot_log_lines();
        let start = log_lines.len().saturating_sub(limit);
        println!("log_count={}", log_lines.len());
        for (index, line) in log_lines.iter().enumerate().skip(start) {
            println!("log[{}]={}", index, line);
        }
        Ok(())
    }

    fn create_disk(&mut self, args: &[&str]) -> AppResult<()> {
        let request = parse_create_disk_args(args)?;
        let disk_size_bytes = request
            .disk_size_mib
            .checked_mul(1024 * 1024)
            .ok_or_else(|| "disk-size-overflow".to_string())?;
        let target_id = request
            .target_id
            .unwrap_or_else(|| self.context.find_first_free_target());
        if target_id >= YUMEDISK_MAX_TARGETS {
            return Err("no-free-target".to_string());
        }

        let dense_mem = DenseMem::new(disk_size_bytes).map_err(|error| error.to_string())?;
        let disk_config = DiskConfig {
            target_id,
            disk_size_bytes,
            read_only: request.read_only,
            ..DiskConfig::default()
        };

        let mut error_text = String::new();
        if !self.context.create_managed_disk(
            disk_config,
            Box::new(dense_mem.clone()),
            Some(&mut error_text),
        ) {
            if error_text.is_empty() {
                error_text = "create-failed".to_string();
            }
            return Err(error_text);
        }

        self.disks.insert(
            target_id,
            CliDisk {
                dense_mem,
                disk_size_bytes,
                read_only: request.read_only,
            },
        );

        let snapshot = self.wait_for_disk_ready(target_id, DISK_READY_TIMEOUT)?;
        println!(
            "created target={}, disk_bytes={}, read_only={}, visible_path={}, physical_drive={}",
            snapshot.target_id,
            snapshot.disk_size_bytes,
            bool_to_text(snapshot.read_only),
            pending_text(&snapshot.visible_path),
            pending_text(&snapshot.physical_drive_path)
        );
        Ok(())
    }

    fn remove_disk(&mut self, args: &[&str]) -> AppResult<()> {
        let Some(arg) = args.first() else {
            return Err("rm requires <target>|all".to_string());
        };

        if arg.eq_ignore_ascii_case("all") {
            if !self.context.remove_all_managed_disks() {
                return Err("remove-all-failed".to_string());
            }
            self.disks.clear();
            println!("removed_all=true");
            return Ok(());
        }

        let target_id = parse_target_value(arg)?;
        let mut error_text = String::new();
        if !self
            .context
            .remove_managed_disk(target_id, Some(&mut error_text))
        {
            if error_text.is_empty() {
                error_text = "remove-failed".to_string();
            }
            return Err(error_text);
        }

        self.disks.remove(&target_id);
        println!("removed target={}", target_id);
        Ok(())
    }

    fn read_disk_bytes(&self, args: &[&str]) -> AppResult<()> {
        if args.len() != 3 {
            return Err("read requires <target> <offset> <length>".to_string());
        }

        let target_id = parse_target_value(args[0])?;
        let offset_bytes = parse_u64_value(args[1], "offset")?;
        let length = parse_usize_value(args[2], "length")?;
        if length == 0 {
            return Err("length must be > 0".to_string());
        }

        let snapshot = self.wait_for_disk_ready(target_id, DISK_READY_TIMEOUT)?;
        validate_range(snapshot.disk_size_bytes, offset_bytes, length)?;

        let buffer = read_device_range(
            &snapshot.physical_drive_path,
            snapshot.sector_size,
            offset_bytes,
            length,
        )?;

        let memory_match = if let Some(local_disk) = self.disks.get(&target_id) {
            let expected = local_disk
                .dense_mem
                .snapshot_range(offset_bytes, length)
                .map_err(|error| error.to_string())?;
            expected == buffer
        } else {
            false
        };

        println!(
            "read target={}, offset={}, bytes={}, data={}, memory_match={}",
            target_id,
            offset_bytes,
            buffer.len(),
            encode_hex_bytes(&buffer),
            bool_to_text(memory_match)
        );
        Ok(())
    }

    fn write_disk_bytes(&self, args: &[&str]) -> AppResult<()> {
        if args.len() != 3 {
            return Err("write requires <target> <offset> <hex-bytes>".to_string());
        }

        let target_id = parse_target_value(args[0])?;
        let offset_bytes = parse_u64_value(args[1], "offset")?;
        let data = decode_hex_bytes(args[2])?;
        if data.is_empty() {
            return Err("hex-bytes must not be empty".to_string());
        }

        let local_disk = self
            .disks
            .get(&target_id)
            .ok_or_else(|| format!("target-not-found: {}", target_id))?;
        if local_disk.read_only {
            return Err(format!("target-read-only: {}", target_id));
        }

        let snapshot = self.wait_for_disk_ready(target_id, DISK_READY_TIMEOUT)?;
        validate_range(snapshot.disk_size_bytes, offset_bytes, data.len())?;

        write_device_range(
            &snapshot.physical_drive_path,
            snapshot.sector_size,
            offset_bytes,
            &data,
        )?;

        let read_back = read_device_range(
            &snapshot.physical_drive_path,
            snapshot.sector_size,
            offset_bytes,
            data.len(),
        )?;
        if read_back != data {
            return Err(format!(
                "readback-mismatch target={} offset={}",
                target_id, offset_bytes
            ));
        }

        wait_for_dense_mem_match(&local_disk.dense_mem, offset_bytes, &data, MEMORY_COMMIT_TIMEOUT)?;

        println!(
            "write_ok target={}, offset={}, bytes={}, data={}, readback=ok, memory_commit=ok",
            target_id,
            offset_bytes,
            data.len(),
            encode_hex_bytes(&data)
        );
        Ok(())
    }

    fn peek_memory_bytes(&self, args: &[&str]) -> AppResult<()> {
        if args.len() != 3 {
            return Err("peek requires <target> <offset> <length>".to_string());
        }

        let target_id = parse_target_value(args[0])?;
        let offset_bytes = parse_u64_value(args[1], "offset")?;
        let length = parse_usize_value(args[2], "length")?;
        if length == 0 {
            return Err("length must be > 0".to_string());
        }

        let local_disk = self
            .disks
            .get(&target_id)
            .ok_or_else(|| format!("target-not-found: {}", target_id))?;
        validate_range(local_disk.disk_size_bytes, offset_bytes, length)?;

        let buffer = local_disk
            .dense_mem
            .snapshot_range(offset_bytes, length)
            .map_err(|error| error.to_string())?;
        println!(
            "peek target={}, offset={}, bytes={}, data={}",
            target_id,
            offset_bytes,
            buffer.len(),
            encode_hex_bytes(&buffer)
        );
        Ok(())
    }

    fn wait_for_disk_ready(
        &self,
        target_id: u32,
        timeout: Duration,
    ) -> AppResult<ManagedDiskSnapshot> {
        let deadline = Instant::now()
            .checked_add(timeout)
            .ok_or_else(|| "timeout-overflow".to_string())?;

        loop {
            if let Some(snapshot) = self.find_disk_snapshot(target_id) {
                if !snapshot.physical_drive_path.is_empty() {
                    return Ok(snapshot);
                }
            }

            if Instant::now() >= deadline {
                return Err(format!("disk-ready-timeout target={}", target_id));
            }

            thread::sleep(Duration::from_millis(100));
        }
    }

    fn find_disk_snapshot(&self, target_id: u32) -> Option<ManagedDiskSnapshot> {
        self.context
            .snapshot_managed_disks()
            .into_iter()
            .find(|disk| disk.target_id == target_id)
    }
}

fn parse_smoke_args(mut args: impl Iterator<Item = String>) -> AppResult<SmokeConfig> {
    let mut config = SmokeConfig::default();

    while let Some(flag) = args.next() {
        let value = args
            .next()
            .ok_or_else(|| format!("missing value for {}", flag))?;
        match flag.as_str() {
            "--disk-mib" => {
                config.disk_mib = value
                    .parse()
                    .map_err(|_| format!("invalid --disk-mib: {}", value))?;
            }
            "--threads" => {
                config.threads = value
                    .parse()
                    .map_err(|_| format!("invalid --threads: {}", value))?;
            }
            "--iterations" => {
                config.iterations_per_thread = value
                    .parse()
                    .map_err(|_| format!("invalid --iterations: {}", value))?;
            }
            "--block-kib" => {
                config.block_kib = value
                    .parse()
                    .map_err(|_| format!("invalid --block-kib: {}", value))?;
            }
            "--target" => {
                let target_id: u32 = value
                    .parse()
                    .map_err(|_| format!("invalid --target: {}", value))?;
                config.target_id = Some(target_id);
            }
            other => {
                return Err(format!("unknown flag: {}", other));
            }
        }
    }

    Ok(config)
}

fn print_usage() {
    println!("usage:");
    println!("  rust-cli");
    println!("  rust-cli shell");
    println!(
        "  rust-cli smoke [--disk-mib N] [--threads N] [--iterations N] [--block-kib N] [--target N]"
    );
    println!("  rust-cli help");
    println!();
    println!("default command: shell");
    println!();
    print_runtime_help();
}

fn print_runtime_help() {
    println!("commands:");
    println!("  help                               show this help");
    println!("  state                              print AppKernel session state");
    println!("  ct <disk-size-mib> [dense|auto] [true|false] [target]");
    println!("                                     create one denseMem disk");
    println!("  rm <target>                        remove one disk target");
    println!("  rm all                             remove all disk targets");
    println!("  ls                                 list managed targets and visible YumeDisk disks");
    println!("  stats                              print aggregated AppKernel counters");
    println!("  wr <target> <offset> <hex-bytes>   write bytes through PhysicalDrive and verify commit");
    println!("  rd <target> <offset> <length>      read bytes through PhysicalDrive");
    println!("  peek <target> <offset> <length>    read bytes from denseMem backing");
    println!("  logs [count]                       print recent backend log lines");
    println!("  exit                               close session and quit");
}

fn parse_create_disk_args(args: &[&str]) -> AppResult<CreateDiskRequest> {
    let Some(first) = args.first() else {
        return Err("ct requires <disk-size-mib> [dense|auto] [true|false] [target]".to_string());
    };

    let disk_size_mib = parse_u64_value(first, "disk size mib")?;
    if disk_size_mib == 0 {
        return Err("disk size mib must be > 0".to_string());
    }

    let mut read_only = false;
    let mut target_id = None;
    let mut mode_seen = false;
    let mut read_only_seen = false;
    let mut target_seen = false;

    for token in args.iter().skip(1) {
        if token.eq_ignore_ascii_case("dense") || token.eq_ignore_ascii_case("auto") {
            if mode_seen {
                return Err("duplicate media mode".to_string());
            }
            mode_seen = true;
            continue;
        }
        if token.eq_ignore_ascii_case("sparse") {
            return Err("sparse not supported; rust-cli currently only supports denseMem".to_string());
        }
        if token.eq_ignore_ascii_case("true") || token.eq_ignore_ascii_case("false") {
            if read_only_seen {
                return Err("duplicate readOnly".to_string());
            }
            read_only = token.eq_ignore_ascii_case("true");
            read_only_seen = true;
            continue;
        }

        let parsed_target = parse_target_value(token)?;
        if target_seen {
            return Err("duplicate target".to_string());
        }
        target_id = Some(parsed_target);
        target_seen = true;
    }

    Ok(CreateDiskRequest {
        disk_size_mib,
        read_only,
        target_id,
    })
}

fn parse_target_value(text: &str) -> AppResult<u32> {
    let value = parse_u32_value(text, "target")?;
    if value > YUMEDISK_MAX_USABLE_TARGET_ID {
        return Err(format!("target out of range: {}", text));
    }
    Ok(value)
}

fn parse_u32_value(text: &str, name: &str) -> AppResult<u32> {
    text.parse::<u32>()
        .map_err(|_| format!("invalid {}: {}", name, text))
}

fn parse_u64_value(text: &str, name: &str) -> AppResult<u64> {
    text.parse::<u64>()
        .map_err(|_| format!("invalid {}: {}", name, text))
}

fn parse_usize_value(text: &str, name: &str) -> AppResult<usize> {
    text.parse::<usize>()
        .map_err(|_| format!("invalid {}: {}", name, text))
}

fn validate_range(total_bytes: u64, offset_bytes: u64, length: usize) -> AppResult<()> {
    let end = offset_bytes
        .checked_add(length as u64)
        .ok_or_else(|| "range-overflow".to_string())?;
    if end > total_bytes {
        return Err(format!(
            "range-out-of-bounds offset={} length={} total={}",
            offset_bytes, length, total_bytes
        ));
    }
    Ok(())
}

fn compute_aligned_range(offset_bytes: u64, length: usize, sector_size: u32) -> AppResult<AlignedRange> {
    if sector_size == 0 {
        return Err("sector-size-zero".to_string());
    }

    let sector_size = u64::from(sector_size);
    let end_bytes = offset_bytes
        .checked_add(length as u64)
        .ok_or_else(|| "range-overflow".to_string())?;
    let aligned_offset = (offset_bytes / sector_size) * sector_size;
    let aligned_end = end_bytes
        .checked_add(sector_size - 1)
        .ok_or_else(|| "range-overflow".to_string())?
        / sector_size
        * sector_size;
    let aligned_length = usize::try_from(
        aligned_end
            .checked_sub(aligned_offset)
            .ok_or_else(|| "range-overflow".to_string())?,
    )
    .map_err(|_| "aligned-range-too-large".to_string())?;
    let inner_offset = usize::try_from(
        offset_bytes
            .checked_sub(aligned_offset)
            .ok_or_else(|| "range-overflow".to_string())?,
    )
    .map_err(|_| "aligned-range-too-large".to_string())?;

    Ok(AlignedRange {
        aligned_offset,
        aligned_length,
        inner_offset,
    })
}

fn read_device_range(
    path: &str,
    sector_size: u32,
    offset_bytes: u64,
    length: usize,
) -> AppResult<Vec<u8>> {
    let aligned_range = compute_aligned_range(offset_bytes, length, sector_size)?;
    let handle = DeviceHandle::open(path, false)?;
    let mut aligned_buffer = vec![0u8; aligned_range.aligned_length];
    handle.read_exact_at(aligned_range.aligned_offset, &mut aligned_buffer)?;

    Ok(aligned_buffer
        [aligned_range.inner_offset..aligned_range.inner_offset + length]
        .to_vec())
}

fn write_device_range(
    path: &str,
    sector_size: u32,
    offset_bytes: u64,
    data: &[u8],
) -> AppResult<()> {
    let aligned_range = compute_aligned_range(offset_bytes, data.len(), sector_size)?;
    let handle = DeviceHandle::open(path, true)?;
    let mut aligned_buffer = vec![0u8; aligned_range.aligned_length];
    handle.read_exact_at(aligned_range.aligned_offset, &mut aligned_buffer)?;

    aligned_buffer[aligned_range.inner_offset..aligned_range.inner_offset + data.len()]
        .copy_from_slice(data);
    handle.write_all_at(aligned_range.aligned_offset, &aligned_buffer)?;
    Ok(())
}

fn wait_for_dense_mem_match(
    dense_mem: &DenseMem,
    offset_bytes: u64,
    data: &[u8],
    timeout: Duration,
) -> AppResult<()> {
    let deadline = Instant::now()
        .checked_add(timeout)
        .ok_or_else(|| "timeout-overflow".to_string())?;

    loop {
        let actual = dense_mem
            .snapshot_range(offset_bytes, data.len())
            .map_err(|error| error.to_string())?;
        if actual == data {
            return Ok(());
        }

        if Instant::now() >= deadline {
            return Err(format!(
                "memory-commit-timeout offset={} bytes={}",
                offset_bytes,
                data.len()
            ));
        }

        thread::sleep(MEMORY_COMMIT_POLL);
    }
}

fn decode_hex_bytes(text: &str) -> AppResult<Vec<u8>> {
    let compact = text
        .strip_prefix("0x")
        .or_else(|| text.strip_prefix("0X"))
        .unwrap_or(text)
        .chars()
        .filter(|ch| !matches!(ch, '_' | '-' | ':' | ' '))
        .collect::<String>();

    if compact.is_empty() {
        return Ok(Vec::new());
    }
    if compact.len() % 2 != 0 {
        return Err("hex-bytes must contain an even number of digits".to_string());
    }

    let mut bytes = Vec::with_capacity(compact.len() / 2);
    let raw = compact.as_bytes();
    let mut index = 0usize;
    while index < raw.len() {
        let pair = std::str::from_utf8(&raw[index..index + 2])
            .map_err(|_| "invalid utf8 in hex data".to_string())?;
        let value = u8::from_str_radix(pair, 16)
            .map_err(|_| format!("invalid hex byte: {}", pair))?;
        bytes.push(value);
        index += 2;
    }
    Ok(bytes)
}

fn encode_hex_bytes(bytes: &[u8]) -> String {
    let mut text = String::with_capacity(bytes.len() * 2);
    for byte in bytes {
        text.push_str(&format!("{:02x}", byte));
    }
    text
}

fn pending_text(text: &str) -> &str {
    if text.is_empty() {
        "<pending-enumeration>"
    } else {
        text
    }
}

fn bool_to_text(value: bool) -> &'static str {
    if value { "true" } else { "false" }
}
