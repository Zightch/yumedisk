use std::io;
use std::io::Write;
use std::sync::Arc;
use std::sync::Mutex;
use std::sync::atomic::AtomicBool;
use std::sync::atomic::Ordering;
use std::thread;
use std::time::Duration;

use backend_rust::YUMEDISK_MAX_USABLE_TARGET_ID;

use super::host::AppResult;
use super::host::CliHost;
use super::host::CreateLocalDiskRequest;
use super::host::CreateLocalDiskSource;
use super::host::RemoveSharedMemorySelector;
use super::host::RemoveTargetSelector;
use crate::NetworkCacheDefaults;

const CACHE_CONFIG_USAGE: &str =
    "cc requires [fifo=<blocks>] [lru=<blocks>] [temp=<files>] [block=<bytes>]";

enum LoopControl {
    Continue,
    Exit,
}

pub fn run_shell() -> AppResult<()> {
    let host = Arc::new(Mutex::new(CliHost::open()?));
    let reaper = start_dead_network_reaper(Arc::clone(&host));
    println!("state=ready(rust-cli)");
    println!(
        "{}",
        host.lock()
            .expect("host poisoned")
            .query_session_state_text()
    );
    print_runtime_help();

    let loop_result = run_command_loop(&host);
    stop_dead_network_reaper(reaper);
    host.lock().expect("host poisoned").shutdown();
    loop_result
}

pub fn run_shell_with_startup_command(planned: super::command::PlannedCommand) -> AppResult<()> {
    let host = Arc::new(Mutex::new(CliHost::open()?));
    let reaper = start_dead_network_reaper(Arc::clone(&host));
    println!("state=ready(rust-cli)");
    println!(
        "{}",
        host.lock()
            .expect("host poisoned")
            .query_session_state_text()
    );

    let args = planned.args.iter().map(String::as_str).collect::<Vec<_>>();
    let mut host_guard = host.lock().expect("host poisoned");
    match execute_command(&mut host_guard, planned.name, args.as_slice()) {
        Ok(LoopControl::Continue) => {
            drop(host_guard);
            print_runtime_help();
            let loop_result = run_command_loop(&host);
            stop_dead_network_reaper(reaper);
            host.lock().expect("host poisoned").shutdown();
            loop_result
        }
        Ok(LoopControl::Exit) => {
            drop(host_guard);
            stop_dead_network_reaper(reaper);
            host.lock().expect("host poisoned").shutdown();
            Ok(())
        }
        Err(error) => {
            drop(host_guard);
            stop_dead_network_reaper(reaper);
            host.lock().expect("host poisoned").shutdown();
            Err(error)
        }
    }
}

fn run_command_loop(host: &Arc<Mutex<CliHost>>) -> AppResult<()> {
    let stdin = io::stdin();

    loop {
        reap_host_disk_events(host);
        reap_host_responses(host);
        reap_host_session_notices(host);
        reap_network_data_changed(host);
        reap_dead_network_disks(host);
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

        let mut host_guard = host.lock().expect("host poisoned");
        match execute_command(&mut host_guard, tokens[0], &tokens[1..]) {
            Ok(LoopControl::Continue) => {
                drop(host_guard);
                reap_host_disk_events(host);
                reap_host_responses(host);
                reap_host_session_notices(host);
                reap_network_data_changed(host);
                reap_dead_network_disks(host);
            }
            Ok(LoopControl::Exit) => {
                drop(host_guard);
                return Ok(());
            }
            Err(error) => eprintln!("error: {}", error),
        }
    }
}

fn execute_command(host: &mut CliHost, command: &str, args: &[&str]) -> AppResult<LoopControl> {
    match command.to_ascii_lowercase().as_str() {
        "help" => {
            print_runtime_help();
            Ok(LoopControl::Continue)
        }
        "query" => {
            println!("{}", host.query_session_state_text());
            print_stats(host)?;
            Ok(LoopControl::Continue)
        }
        "stats" => {
            print_stats(host)?;
            Ok(LoopControl::Continue)
        }
        "ls" => {
            print_managed_disks(host);
            Ok(LoopControl::Continue)
        }
        "debug" => {
            print_debug(host)?;
            Ok(LoopControl::Continue)
        }
        "dbg-read" => {
            let request = parse_debug_read_args(args)?;
            let bytes =
                host.debug_read_target_bytes(request.target_id, request.offset, request.length)?;
            println!(
                "dbg-read target={}, offset={}, length={}, hex={}",
                request.target_id,
                request.offset,
                bytes.len(),
                encode_hex(&bytes)
            );
            Ok(LoopControl::Continue)
        }
        "dbg-write" => {
            let request = parse_debug_write_args(args)?;
            host.debug_write_target_bytes(request.target_id, request.offset, &request.bytes)?;
            println!(
                "dbg-write target={}, offset={}, length={}",
                request.target_id,
                request.offset,
                request.bytes.len()
            );
            Ok(LoopControl::Continue)
        }
        "sm" => {
            if args.len() != 1 {
                return Err("sm requires <size-mib>".to_string());
            }
            let size_mib = parse_u64_value(args.first().copied(), "size mib")?;
            let result = host.create_shared_memory(size_mib)?;
            println!(
                "created smid={}, size_bytes={}",
                result.smid, result.size_bytes
            );
            Ok(LoopControl::Continue)
        }
        "ct" => {
            let request = parse_create_local_disk_args(args)?;
            let target_id = host.create_local_disk(request)?;
            println!("created target={}", target_id);
            Ok(LoopControl::Continue)
        }
        "auth" => {
            let (addr, claim_code, target_id) = parse_auth_args(args)?;
            let result = host.mount_network_disk(addr, claim_code, target_id)?;
            println!(
                "mounted target={}, addr={}, disk_id={}, session_id={}, disk_bytes={}, read_only={}, raw_limit_bytes={}",
                result.target_id,
                result.addr,
                result.disk_id,
                result.session_id,
                result.disk_size_bytes,
                bool_to_text(result.read_only),
                network_core::protocol::MAX_DATA_PLANE_RAW_BYTES
            );
            Ok(LoopControl::Continue)
        }
        "cc" => {
            let command = parse_cache_config_args(args, host.network_cache_defaults())?;
            match command {
                CacheConfigCommand::Show => {
                    print_cache_config("cache_config", host.network_cache_defaults());
                }
                CacheConfigCommand::Update(defaults) => {
                    host.set_network_cache_defaults(defaults);
                    print_cache_config("cache_config_updated", defaults);
                }
            }
            Ok(LoopControl::Continue)
        }
        "rm" => {
            let selector = parse_remove_args(args)?;
            match selector {
                RemoveCommand::Target(target_selector) => {
                    let removed = host.remove_targets(target_selector)?;
                    println!("removed_targets={}", removed.len());
                }
                RemoveCommand::SharedMemory(smid_selector) => {
                    let removed = host.remove_shared_memory(smid_selector)?;
                    println!("removed_smid={}", removed.len());
                }
            }
            Ok(LoopControl::Continue)
        }
        "exit" | "quit" => Ok(LoopControl::Exit),
        other => Err(format!("unknown command: {}", other)),
    }
}

enum RemoveCommand {
    Target(RemoveTargetSelector),
    SharedMemory(RemoveSharedMemorySelector),
}

#[derive(Debug, Clone, PartialEq, Eq)]
enum CacheConfigCommand {
    Show,
    Update(NetworkCacheDefaults),
}

#[derive(Debug, Clone, PartialEq, Eq)]
struct DebugReadCommand {
    target_id: u32,
    offset: u64,
    length: usize,
}

#[derive(Debug, Clone, PartialEq, Eq)]
struct DebugWriteCommand {
    target_id: u32,
    offset: u64,
    bytes: Vec<u8>,
}

fn parse_remove_args(args: &[&str]) -> AppResult<RemoveCommand> {
    if args.len() != 1 {
        return Err("rm requires target=<id>|target=all|smid=<id>|smid=all".to_string());
    }
    let Some(arg) = args.first().copied() else {
        return Err("rm requires target=<id>|target=all|smid=<id>|smid=all".to_string());
    };
    let (key, value) = split_key_value(arg)?;
    match key {
        "target" => {
            if value.eq_ignore_ascii_case("all") {
                Ok(RemoveCommand::Target(RemoveTargetSelector::All))
            } else {
                Ok(RemoveCommand::Target(RemoveTargetSelector::One(
                    parse_target_value(value)?,
                )))
            }
        }
        "smid" => {
            if value.eq_ignore_ascii_case("all") {
                Ok(RemoveCommand::SharedMemory(RemoveSharedMemorySelector::All))
            } else {
                Ok(RemoveCommand::SharedMemory(
                    RemoveSharedMemorySelector::One(parse_u64_value(Some(value), "smid")?),
                ))
            }
        }
        _ => Err("rm requires target=<id>|target=all|smid=<id>|smid=all".to_string()),
    }
}

fn parse_debug_read_args(args: &[&str]) -> AppResult<DebugReadCommand> {
    if args.len() != 3 {
        return Err("dbg-read requires target=<id> offset=<bytes> length=<bytes>".to_string());
    }
    let mut target_id = None;
    let mut offset = None;
    let mut length = None;
    for token in args {
        let (key, value) = split_key_value(token)?;
        match key {
            "target" => target_id = Some(parse_target_value(value)?),
            "offset" => offset = Some(parse_u64_value(Some(value), "offset")?),
            "length" => length = Some(parse_length_value(value)?),
            _ => {
                return Err(
                    "dbg-read requires target=<id> offset=<bytes> length=<bytes>".to_string(),
                );
            }
        }
    }
    Ok(DebugReadCommand {
        target_id: target_id.ok_or_else(|| {
            "dbg-read requires target=<id> offset=<bytes> length=<bytes>".to_string()
        })?,
        offset: offset.ok_or_else(|| {
            "dbg-read requires target=<id> offset=<bytes> length=<bytes>".to_string()
        })?,
        length: length.ok_or_else(|| {
            "dbg-read requires target=<id> offset=<bytes> length=<bytes>".to_string()
        })?,
    })
}

fn parse_debug_write_args(args: &[&str]) -> AppResult<DebugWriteCommand> {
    if args.len() != 3 {
        return Err("dbg-write requires target=<id> offset=<bytes> hex=<...>".to_string());
    }
    let mut target_id = None;
    let mut offset = None;
    let mut bytes = None;
    for token in args {
        let (key, value) = split_key_value(token)?;
        match key {
            "target" => target_id = Some(parse_target_value(value)?),
            "offset" => offset = Some(parse_u64_value(Some(value), "offset")?),
            "hex" => bytes = Some(parse_hex_bytes(value)?),
            _ => return Err("dbg-write requires target=<id> offset=<bytes> hex=<...>".to_string()),
        }
    }
    Ok(DebugWriteCommand {
        target_id: target_id
            .ok_or_else(|| "dbg-write requires target=<id> offset=<bytes> hex=<...>".to_string())?,
        offset: offset
            .ok_or_else(|| "dbg-write requires target=<id> offset=<bytes> hex=<...>".to_string())?,
        bytes: bytes
            .ok_or_else(|| "dbg-write requires target=<id> offset=<bytes> hex=<...>".to_string())?,
    })
}

fn parse_cache_config_args(
    args: &[&str],
    current: NetworkCacheDefaults,
) -> AppResult<CacheConfigCommand> {
    if args.is_empty() {
        return Ok(CacheConfigCommand::Show);
    }

    let mut next = current;
    let mut fifo_seen = false;
    let mut lru_seen = false;
    let mut temp_seen = false;
    let mut block_seen = false;

    for token in args {
        let (key, value) = split_key_value(token)?;
        match key {
            "fifo" => {
                if fifo_seen {
                    return Err("duplicate fifo".to_string());
                }
                next.fifo_capacity_blocks = parse_nonzero_usize_value(value, "fifo")?;
                fifo_seen = true;
            }
            "lru" => {
                if lru_seen {
                    return Err("duplicate lru".to_string());
                }
                next.lru_capacity_blocks = parse_nonzero_usize_value(value, "lru")?;
                lru_seen = true;
            }
            "temp" => {
                if temp_seen {
                    return Err("duplicate temp".to_string());
                }
                next.temp_max_files = parse_nonzero_usize_value(value, "temp")?;
                temp_seen = true;
            }
            "block" => {
                if block_seen {
                    return Err("duplicate block".to_string());
                }
                next.block_size_bytes = parse_nonzero_u32_value(value, "block")?;
                block_seen = true;
            }
            _ => return Err(CACHE_CONFIG_USAGE.to_string()),
        }
    }

    Ok(CacheConfigCommand::Update(next))
}

fn print_cache_config(prefix: &str, defaults: NetworkCacheDefaults) {
    println!(
        "{} fifo_blocks={}, lru_blocks={}, temp_max_files={}, block_size_bytes={}, future_mounts_only=true",
        prefix,
        defaults.fifo_capacity_blocks,
        defaults.lru_capacity_blocks,
        defaults.temp_max_files,
        defaults.block_size_bytes
    );
}

fn print_stats(host: &CliHost) -> AppResult<()> {
    let stats = host.query_backend_stats()?;
    println!(
        "heartbeat_sent={}, command_failures={}, protocol_failures={}, responses_queued={}, responses_dropped={}, session_notices_queued={}, session_notices_dropped={}, disk_count={}",
        stats.heartbeat_sent,
        stats.command_failures,
        stats.protocol_failures,
        stats.responses_queued,
        stats.responses_dropped,
        stats.session_notices_queued,
        stats.session_notices_dropped,
        stats.disk_count
    );
    Ok(())
}

fn print_debug(host: &CliHost) -> AppResult<()> {
    let snapshot = host.query_debug_snapshot()?;
    println!("debug_session {}", snapshot.session_state_text);
    println!(
        "debug_stats heartbeat_sent={}, command_failures={}, protocol_failures={}, responses_queued={}, responses_dropped={}, session_notices_queued={}, session_notices_dropped={}, disk_count={}",
        snapshot.stats.heartbeat_sent,
        snapshot.stats.command_failures,
        snapshot.stats.protocol_failures,
        snapshot.stats.responses_queued,
        snapshot.stats.responses_dropped,
        snapshot.stats.session_notices_queued,
        snapshot.stats.session_notices_dropped,
        snapshot.stats.disk_count
    );
    for disk in snapshot.disks {
        println!(
            "debug_disk target={}, disk_bytes={}, sector_size={}, read_only={}, lifecycle={}, online={}",
            disk.target_id,
            disk.disk_size_bytes,
            disk.sector_size,
            bool_to_text(disk.read_only),
            disk.lifecycle_text,
            bool_to_text(disk.online)
        );
    }
    for mount in host.network_mounts() {
        println!(
            "debug_network_mount target={}, addr={}, disk_id={}, session_id={}, read_only={}, raw_limit_bytes={}",
            mount.target_id,
            mount.addr,
            mount.disk_id,
            mount.session_id,
            bool_to_text(mount.read_only),
            network_core::protocol::MAX_DATA_PLANE_RAW_BYTES
        );
    }
    for shared in host.snapshot_shared_memory() {
        println!(
            "debug_shared_memory smid={}, size_bytes={}, bound_targets={}",
            shared.smid,
            shared.size_bytes,
            format_target_list(&shared.bound_targets)
        );
    }
    Ok(())
}

fn print_managed_disks(host: &CliHost) {
    let managed_disks = host.snapshot_managed_disks();
    println!("managed_target_count={}", managed_disks.len());
    for disk in managed_disks {
        println!(
            "target={}, disk_bytes={}, sector_size={}, read_only={}, lifecycle={}, online={}",
            disk.target_id,
            disk.disk_size_bytes,
            disk.sector_size,
            bool_to_text(disk.read_only),
            disk.lifecycle_text,
            bool_to_text(disk.online)
        );
    }
}

pub fn print_runtime_help() {
    println!("commands:");
    println!("  help                               show this help");
    println!("  query                              print AppKernel session state");
    println!("  auth <addr> <claim_code> [target]  authenticate and mount one network disk");
    println!("  cc [fifo=<n>] [lru=<n>] [temp=<n>] [block=<bytes>]");
    println!(
        "                                     show or update default cache config for future network mounts"
    );
    println!("  sm <size-mib>                      create one shared memory area");
    println!("  ct size=<mib> [ro=<true|false>] [target=<id>]");
    println!("                                     create one denseMem disk");
    println!("  ct smid=<id> [ro=<true|false>] [target=<id>]");
    println!("                                     create one disk from shared memory");
    println!("  rm target=<id>                     remove one disk target");
    println!("  rm target=all                      remove all disk targets");
    println!("  rm smid=<id>                       remove one shared memory area");
    println!("  rm smid=all                        remove all shared memory areas");
    println!("  ls                                 list managed targets");
    println!("  stats                              print aggregated AppKernel counters");
    println!("  debug                              print backend snapshot");
    println!("  dbg-read target=<id> offset=<bytes> length=<bytes>");
    println!("                                     read raw bytes from shared local backing");
    println!("  dbg-write target=<id> offset=<bytes> hex=<...>");
    println!("                                     write raw bytes into shared local backing");
    println!("  exit                               close session and quit");
}

fn parse_auth_args<'a>(args: &'a [&'a str]) -> AppResult<(&'a str, &'a str, Option<u32>)> {
    let Some(addr) = args.first().copied() else {
        return Err("auth requires <addr> <claim_code> [target]".to_string());
    };
    let Some(claim_code) = args.get(1).copied() else {
        return Err("auth requires <addr> <claim_code> [target]".to_string());
    };
    let target_id = match args.get(2) {
        Some(value) => Some(parse_target_value(value)?),
        None => None,
    };
    Ok((addr, claim_code, target_id))
}

fn parse_create_local_disk_args(args: &[&str]) -> AppResult<CreateLocalDiskRequest> {
    let Some(first) = args.first().copied() else {
        return Err("ct requires size=<mib>|smid=<id> [ro=<true|false>] [target=<id>]".to_string());
    };

    let (key, value) = split_key_value(first)?;
    let source = match key {
        "size" => CreateLocalDiskSource::SizeMib(parse_u64_value(Some(value), "size mib")?),
        "smid" => CreateLocalDiskSource::SharedMemoryId(parse_u64_value(Some(value), "smid")?),
        _ => {
            return Err(
                "ct requires size=<mib>|smid=<id> [ro=<true|false>] [target=<id>]".to_string(),
            );
        }
    };

    let mut read_only = false;
    let mut target_id = None;
    let mut read_only_seen = false;
    let mut target_seen = false;

    for token in args.iter().skip(1) {
        let (key, value) = split_key_value(token)?;
        match key {
            "ro" => {
                if read_only_seen {
                    return Err("duplicate ro".to_string());
                }
                read_only = parse_bool_value(value)?;
                read_only_seen = true;
            }
            "target" => {
                if target_seen {
                    return Err("duplicate target".to_string());
                }
                target_id = Some(parse_target_value(value)?);
                target_seen = true;
            }
            _ => {
                return Err(
                    "ct requires size=<mib>|smid=<id> [ro=<true|false>] [target=<id>]".to_string(),
                );
            }
        }
    }

    Ok(CreateLocalDiskRequest {
        source,
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

fn parse_u64_value(text: Option<&str>, name: &str) -> AppResult<u64> {
    let Some(text) = text else {
        return Err(format!("missing {}", name));
    };
    text.parse::<u64>()
        .map_err(|_| format!("invalid {}: {}", name, text))
}

fn parse_usize_value(text: &str, name: &str) -> AppResult<usize> {
    let value = parse_u64_value(Some(text), name)?;
    usize::try_from(value).map_err(|_| format!("{} too large: {}", name, text))
}

fn parse_nonzero_usize_value(text: &str, name: &str) -> AppResult<usize> {
    let value = parse_usize_value(text, name)?;
    if value == 0 {
        return Err(format!("{} must be greater than 0", name));
    }
    Ok(value)
}

fn parse_nonzero_u32_value(text: &str, name: &str) -> AppResult<u32> {
    let value = parse_u64_value(Some(text), name)?;
    let value = u32::try_from(value).map_err(|_| format!("{} too large: {}", name, text))?;
    if value == 0 {
        return Err(format!("{} must be greater than 0", name));
    }
    Ok(value)
}

fn parse_bool_value(text: &str) -> AppResult<bool> {
    match text {
        "true" => Ok(true),
        "false" => Ok(false),
        other => Err(format!("invalid bool: {}", other)),
    }
}

fn split_key_value(text: &str) -> AppResult<(&str, &str)> {
    let Some((key, value)) = text.split_once('=') else {
        return Err(format!("invalid argument: {}", text));
    };
    Ok((key, value))
}

fn parse_length_value(value: &str) -> AppResult<usize> {
    let length = parse_u64_value(Some(value), "length")?;
    usize::try_from(length).map_err(|_| "length-too-large".to_string())
}

fn parse_hex_bytes(text: &str) -> AppResult<Vec<u8>> {
    let normalized = text
        .strip_prefix("0x")
        .or_else(|| text.strip_prefix("0X"))
        .unwrap_or(text)
        .bytes()
        .filter(|byte| !matches!(byte, b'-' | b'_' | b':'))
        .collect::<Vec<_>>();
    if normalized.is_empty() {
        return Err("hex must not be empty".to_string());
    }
    if normalized.len() % 2 != 0 {
        return Err("hex length must be even".to_string());
    }
    let mut bytes = Vec::with_capacity(normalized.len() / 2);
    for pair in normalized.chunks_exact(2) {
        let high = parse_hex_nibble(pair[0])?;
        let low = parse_hex_nibble(pair[1])?;
        bytes.push((high << 4) | low);
    }
    Ok(bytes)
}

fn parse_hex_nibble(value: u8) -> AppResult<u8> {
    match value {
        b'0'..=b'9' => Ok(value - b'0'),
        b'a'..=b'f' => Ok(value - b'a' + 10),
        b'A'..=b'F' => Ok(value - b'A' + 10),
        _ => Err(format!("invalid hex digit: {}", char::from(value))),
    }
}

fn bool_to_text(value: bool) -> &'static str {
    if value { "true" } else { "false" }
}

fn encode_hex(bytes: &[u8]) -> String {
    let mut text = String::with_capacity(bytes.len() * 2);
    for byte in bytes {
        use std::fmt::Write as _;
        let _ = write!(&mut text, "{:02X}", byte);
    }
    text
}

fn format_target_list(targets: &[u32]) -> String {
    if targets.is_empty() {
        return "none".to_string();
    }
    targets
        .iter()
        .map(|target_id| target_id.to_string())
        .collect::<Vec<_>>()
        .join(",")
}

fn reap_host_responses(host: &Arc<Mutex<CliHost>>) {
    loop {
        let result = host
            .lock()
            .expect("host poisoned")
            .poll_managed_disk_response();
        match result {
            Ok(Some(_response)) => {}
            Ok(None) => break,
            Err(error) => {
                eprintln!("error: host-response-poll-failed: {}", error);
                break;
            }
        }
    }
}

fn reap_host_disk_events(host: &Arc<Mutex<CliHost>>) {
    loop {
        let result = host
            .lock()
            .expect("host poisoned")
            .poll_managed_disk_event();
        match result {
            Ok(Some(event)) => {
                eprintln!(
                    "notice: disk-event target={}, type={:?}",
                    event.event.target_id, event.event.event_type
                );
            }
            Ok(None) => break,
            Err(error) => {
                eprintln!("error: host-disk-event-poll-failed: {}", error);
                break;
            }
        }
    }
}

fn reap_host_session_notices(host: &Arc<Mutex<CliHost>>) {
    loop {
        let result = host
            .lock()
            .expect("host poisoned")
            .poll_managed_session_notice();
        match result {
            Ok(Some(_notice)) => {}
            Ok(None) => break,
            Err(error) => {
                eprintln!("error: host-session-notice-poll-failed: {}", error);
                break;
            }
        }
    }
}

fn reap_dead_network_disks(host: &Arc<Mutex<CliHost>>) {
    match host
        .lock()
        .expect("host poisoned")
        .reap_dead_network_disks()
    {
        Ok(removed) => {
            for target_id in removed {
                eprintln!("notice: removed dead network target={}", target_id);
            }
        }
        Err(error) => eprintln!("error: auto-remove-dead-network-disks: {}", error),
    }
}

fn reap_network_data_changed(host: &Arc<Mutex<CliHost>>) {
    if let Err(error) = host
        .lock()
        .expect("host poisoned")
        .apply_network_data_changed()
    {
        eprintln!("error: apply-network-data-changed: {}", error);
    }
}

struct DeadNetworkReaper {
    stop: Arc<AtomicBool>,
    thread: thread::JoinHandle<()>,
}

fn start_dead_network_reaper(host: Arc<Mutex<CliHost>>) -> DeadNetworkReaper {
    let stop = Arc::new(AtomicBool::new(false));
    let stop_flag = Arc::clone(&stop);
    let thread = thread::spawn(move || {
        while !stop_flag.load(Ordering::Acquire) {
            reap_host_disk_events(&host);
            reap_host_responses(&host);
            reap_host_session_notices(&host);
            reap_network_data_changed(&host);
            reap_dead_network_disks(&host);
            thread::sleep(Duration::from_millis(200));
        }
    });
    DeadNetworkReaper { stop, thread }
}

fn stop_dead_network_reaper(reaper: DeadNetworkReaper) {
    reaper.stop.store(true, Ordering::Release);
    let _ = reaper.thread.join();
}

#[cfg(test)]
mod tests {
    use super::CacheConfigCommand;
    use super::CreateLocalDiskSource;
    use super::DebugReadCommand;
    use super::DebugWriteCommand;
    use super::RemoveCommand;
    use super::RemoveSharedMemorySelector;
    use super::RemoveTargetSelector;
    use super::parse_auth_args;
    use super::parse_cache_config_args;
    use super::parse_create_local_disk_args;
    use super::parse_debug_read_args;
    use super::parse_debug_write_args;
    use super::parse_remove_args;
    use crate::NetworkCacheDefaults;

    #[test]
    fn parse_auth_args_accepts_optional_target() {
        let parsed = parse_auth_args(&[
            "127.0.0.1:9736",
            "A1b2C3d4E5f6G7h8abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789ab",
            "3",
        ])
        .expect("parse should succeed");
        assert_eq!(parsed.0, "127.0.0.1:9736");
        assert_eq!(parsed.2, Some(3));
    }

    #[test]
    fn parse_auth_args_requires_addr_and_claim_code() {
        let error = parse_auth_args(&["127.0.0.1:9736"]).expect_err("parse should fail");
        assert_eq!(error, "auth requires <addr> <claim_code> [target]");
    }

    #[test]
    fn parse_create_local_disk_args_accepts_size_and_target() {
        let parsed = parse_create_local_disk_args(&["size=64", "ro=true", "target=5"])
            .expect("parse should succeed");
        match parsed.source {
            CreateLocalDiskSource::SizeMib(size_mib) => assert_eq!(size_mib, 64),
            other => panic!("unexpected source: {:?}", other),
        }
        assert!(parsed.read_only);
        assert_eq!(parsed.target_id, Some(5));
    }

    #[test]
    fn parse_create_local_disk_args_accepts_smid() {
        let parsed = parse_create_local_disk_args(&["smid=7"]).expect("parse should succeed");
        match parsed.source {
            CreateLocalDiskSource::SharedMemoryId(smid) => assert_eq!(smid, 7),
            other => panic!("unexpected source: {:?}", other),
        }
        assert!(!parsed.read_only);
        assert_eq!(parsed.target_id, None);
    }

    #[test]
    fn parse_remove_args_accepts_target_and_smid_selectors() {
        match parse_remove_args(&["target=all"]).expect("target=all should parse") {
            RemoveCommand::Target(RemoveTargetSelector::All) => {}
            _ => panic!("unexpected remove target selector"),
        }

        match parse_remove_args(&["smid=9"]).expect("smid=9 should parse") {
            RemoveCommand::SharedMemory(RemoveSharedMemorySelector::One(smid)) => {
                assert_eq!(smid, 9)
            }
            _ => panic!("unexpected remove shared selector"),
        }
    }

    #[test]
    fn parse_cache_config_args_shows_current_config_when_empty() {
        let parsed = parse_cache_config_args(&[], NetworkCacheDefaults::default())
            .expect("parse should succeed");
        assert_eq!(parsed, CacheConfigCommand::Show);
    }

    #[test]
    fn parse_cache_config_args_updates_partial_fields() {
        let parsed =
            parse_cache_config_args(&["fifo=16", "block=65536"], NetworkCacheDefaults::default())
                .expect("parse should succeed");

        match parsed {
            CacheConfigCommand::Update(defaults) => {
                assert_eq!(defaults.fifo_capacity_blocks, 16);
                assert_eq!(defaults.lru_capacity_blocks, 64);
                assert_eq!(defaults.temp_max_files, 64);
                assert_eq!(defaults.block_size_bytes, 65536);
            }
            other => panic!("unexpected cache config command: {:?}", other),
        }
    }

    #[test]
    fn parse_cache_config_args_rejects_invalid_values() {
        let error = parse_cache_config_args(&["temp=0"], NetworkCacheDefaults::default())
            .expect_err("parse should fail");
        assert_eq!(error, "temp must be greater than 0");
    }

    #[test]
    fn parse_cache_config_args_rejects_unknown_keys() {
        let error = parse_cache_config_args(&["mode=fifo"], NetworkCacheDefaults::default())
            .expect_err("parse should fail");
        assert_eq!(
            error,
            "cc requires [fifo=<blocks>] [lru=<blocks>] [temp=<files>] [block=<bytes>]"
        );
    }

    #[test]
    fn parse_debug_read_args_accepts_target_offset_and_length() {
        let parsed = parse_debug_read_args(&["target=4", "offset=512", "length=16"])
            .expect("parse should succeed");
        assert_eq!(
            parsed,
            DebugReadCommand {
                target_id: 4,
                offset: 512,
                length: 16,
            }
        );
    }

    #[test]
    fn parse_debug_write_args_decodes_hex_payload() {
        let parsed = parse_debug_write_args(&["target=3", "offset=8", "hex=0xAA-bb_cc:dd"])
            .expect("parse should succeed");
        assert_eq!(
            parsed,
            DebugWriteCommand {
                target_id: 3,
                offset: 8,
                bytes: vec![0xAA, 0xBB, 0xCC, 0xDD],
            }
        );
    }
}
