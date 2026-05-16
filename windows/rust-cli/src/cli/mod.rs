mod command;
mod local;
mod local_shell;

use std::ffi::OsString;

use command::CliCommand;

pub fn run_from_env<I>(args: I) -> Result<(), String>
where
    I: IntoIterator<Item = OsString>,
{
    let command = command::parse_args(args)?;
    match command {
        CliCommand::Shell => local_shell::run_shell(),
        CliCommand::Help => {
            print_usage();
            Ok(())
        }
        CliCommand::Network(planned) => Err(format!(
            "network command not implemented yet: {}",
            planned.name
        )),
    }
}

fn print_usage() {
    println!("usage:");
    println!("  rust-cli");
    println!("  rust-cli shell");
    println!("  rust-cli help");
    println!("  rust-cli auth <addr> <claim_code>");
    println!("  rust-cli open <addr> <claim_code>");
    println!("  rust-cli read <addr> <claim_code> <offset> <length>");
    println!("  rust-cli write <addr> <claim_code> <offset> <data>");
    println!("  rust-cli smoke <addr> <claim_code>");
    println!();
    println!("default command: shell");
    println!();
    local_shell::print_runtime_help();
}
