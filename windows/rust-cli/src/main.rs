mod dense_mem;
mod disk_io;
mod stress;

use std::env;
use std::process;

use stress::SmokeConfig;
use stress::run_smoke;

type AppResult<T> = Result<T, String>;

fn main() {
    if let Err(error) = run() {
        eprintln!("error: {}", error);
        process::exit(1);
    }
}

fn run() -> AppResult<()> {
    let mut args = env::args().skip(1);
    match args.next().as_deref() {
        None | Some("smoke") => {
            let config = parse_smoke_args(args)?;
            run_smoke(config)
        }
        Some("help") | Some("--help") | Some("-h") => {
            print_help();
            Ok(())
        }
        Some(command) => Err(format!("unknown command: {}", command)),
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

fn print_help() {
    println!("usage:");
    println!("  rust-cli");
    println!(
        "  rust-cli smoke [--disk-mib N] [--threads N] [--iterations N] [--block-kib N] [--target N]"
    );
    println!("  rust-cli help");
    println!();
    println!("default command: smoke");
}
