use std::io;
use std::io::Write;
use std::sync::Arc;
use std::sync::Mutex;
use std::sync::atomic::AtomicBool;
use std::sync::atomic::Ordering;
use std::thread;
use std::time::Duration;

use backend_rust::YUMEDISK_MAX_USABLE_TARGET_ID;

use super::command::PlannedNetworkCommand;
use super::host::AppResult;
use super::host::CliHost;
use super::host::CreateDenseDiskRequest;

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

pub fn run_shell_with_startup_command(planned: PlannedNetworkCommand) -> AppResult<()> {
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
        "ct" => {
            let request = parse_create_dense_disk_args(args)?;
            let target_id = host.create_dense_disk(request)?;
            println!("created target={}", target_id);
            Ok(LoopControl::Continue)
        }
        "auth" => {
            let (addr, claim_code, target_id) = parse_auth_args(args)?;
            let result = host.mount_network_disk(addr, claim_code, target_id)?;
            println!(
                "mounted target={}, addr={}, disk_id={}, session_id={}, disk_bytes={}, read_only={}, max_io_bytes={}",
                result.target_id,
                result.addr,
                result.disk_id,
                result.session_id,
                result.disk_size_bytes,
                bool_to_text(result.read_only),
                result.max_io_bytes
            );
            Ok(LoopControl::Continue)
        }
        "rm" => {
            let Some(arg) = args.first() else {
                return Err("rm requires <target>|all".to_string());
            };
            if arg.eq_ignore_ascii_case("all") {
                host.remove_all_disks()?;
                println!("removed_all=true");
                return Ok(LoopControl::Continue);
            }

            let target_id = parse_target_value(arg)?;
            host.remove_disk(target_id)?;
            println!("removed target={}", target_id);
            Ok(LoopControl::Continue)
        }
        "exit" | "quit" => Ok(LoopControl::Exit),
        other => Err(format!("unknown command: {}", other)),
    }
}

fn print_stats(host: &CliHost) -> AppResult<()> {
    let stats = host.query_backend_stats()?;
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

fn print_debug(host: &CliHost) -> AppResult<()> {
    let snapshot = host.query_debug_snapshot()?;
    println!("debug_session {}", snapshot.session_state_text);
    println!(
        "debug_stats heartbeat_sent={}, command_failures={}, protocol_failures={}, events_queued={}, events_dropped={}, disk_count={}",
        snapshot.stats.heartbeat_sent,
        snapshot.stats.command_failures,
        snapshot.stats.protocol_failures,
        snapshot.stats.events_queued,
        snapshot.stats.events_dropped,
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
            "debug_network_mount target={}, addr={}, disk_id={}, session_id={}, read_only={}, max_io_bytes={}",
            mount.target_id,
            mount.addr,
            mount.disk_id,
            mount.session_id,
            bool_to_text(mount.read_only),
            mount.max_io_bytes
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
    println!("  ct <disk-size-mib> [dense|auto] [true|false] [target]");
    println!("                                     create one denseMem disk");
    println!("  rm <target>                        remove one disk target");
    println!("  rm all                             remove all disk targets");
    println!("  ls                                 list managed targets");
    println!("  stats                              print aggregated AppKernel counters");
    println!("  debug                              print backend snapshot");
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

fn parse_create_dense_disk_args(args: &[&str]) -> AppResult<CreateDenseDiskRequest> {
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
            return Err(
                "sparse not supported; rust-cli currently only supports denseMem".to_string(),
            );
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

    Ok(CreateDenseDiskRequest {
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

fn bool_to_text(value: bool) -> &'static str {
    if value { "true" } else { "false" }
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

struct DeadNetworkReaper {
    stop: Arc<AtomicBool>,
    thread: thread::JoinHandle<()>,
}

fn start_dead_network_reaper(host: Arc<Mutex<CliHost>>) -> DeadNetworkReaper {
    let stop = Arc::new(AtomicBool::new(false));
    let stop_flag = Arc::clone(&stop);
    let thread = thread::spawn(move || {
        while !stop_flag.load(Ordering::Acquire) {
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
    use super::parse_auth_args;

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
}
