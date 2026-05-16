use std::process;

fn main() {
    if let Err(error) = rust_cli::cli::run_from_env(std::env::args_os()) {
        eprintln!("error: {}", error);
        process::exit(1);
    }
}
