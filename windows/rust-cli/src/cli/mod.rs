mod command;
mod host;
mod local;
mod shell;

use std::ffi::OsString;

use command::CliCommand;

pub fn run_from_env<I>(args: I) -> Result<(), String>
where
    I: IntoIterator<Item = OsString>,
{
    let command = command::parse_args(args)?;
    match command {
        CliCommand::Shell => shell::run_shell(),
        CliCommand::Help => {
            print_usage();
            Ok(())
        }
        CliCommand::Network(planned) => shell::run_shell_with_startup_command(planned),
    }
}

fn print_usage() {
    println!("usage:");
    println!("  rust-cli");
    println!("  rust-cli shell");
    println!("  rust-cli help");
    println!("  rust-cli auth <addr> <claim_code>");
    println!();
    println!("default command: shell");
    println!();
    shell::print_runtime_help();
}
