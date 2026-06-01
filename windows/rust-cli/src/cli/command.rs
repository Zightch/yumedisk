use std::ffi::OsString;

#[derive(Debug, Clone, PartialEq, Eq)]
pub enum CliCommand {
    Shell,
    Help,
    Runtime(PlannedCommand),
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct PlannedCommand {
    pub name: &'static str,
    pub args: Vec<String>,
}

pub fn parse_args<I>(args: I) -> Result<CliCommand, String>
where
    I: IntoIterator<Item = OsString>,
{
    let mut args = args
        .into_iter()
        .map(|value| {
            value
                .into_string()
                .map_err(|_| "non-utf8-argument".to_string())
        })
        .collect::<Result<Vec<_>, _>>()?
        .into_iter();

    let _program = args.next();
    let Some(command) = args.next() else {
        return Ok(CliCommand::Shell);
    };

    match command.as_str() {
        "shell" => Ok(CliCommand::Shell),
        "help" | "--help" | "-h" => Ok(CliCommand::Help),
        "auth" => Ok(CliCommand::Runtime(PlannedCommand {
            name: "auth",
            args: args.collect(),
        })),
        "cc" => Ok(CliCommand::Runtime(PlannedCommand {
            name: "cc",
            args: args.collect(),
        })),
        _ => Err(format!("unknown command: {}", command)),
    }
}

#[cfg(test)]
mod tests {
    use super::CliCommand;
    use super::parse_args;
    use std::ffi::OsString;

    #[test]
    fn defaults_to_shell_when_no_command_is_provided() {
        let command = parse_args(vec![OsString::from("rust-cli")]).expect("parse should succeed");
        assert_eq!(command, CliCommand::Shell);
    }

    #[test]
    fn parses_runtime_command_without_leaking_cli_layer() {
        let command = parse_args(vec![
            OsString::from("rust-cli"),
            OsString::from("auth"),
            OsString::from("127.0.0.1:9000"),
            OsString::from("claim"),
        ])
        .expect("parse should succeed");

        match command {
            CliCommand::Runtime(planned) => {
                assert_eq!(planned.name, "auth");
                assert_eq!(planned.args, vec!["127.0.0.1:9000", "claim"]);
            }
            other => panic!("unexpected command: {:?}", other),
        }
    }

    #[test]
    fn parses_cache_config_runtime_command() {
        let command = parse_args(vec![
            OsString::from("rust-cli"),
            OsString::from("cc"),
            OsString::from("fifo=16"),
            OsString::from("block=65536"),
        ])
        .expect("parse should succeed");

        match command {
            CliCommand::Runtime(planned) => {
                assert_eq!(planned.name, "cc");
                assert_eq!(planned.args, vec!["fifo=16", "block=65536"]);
            }
            other => panic!("unexpected command: {:?}", other),
        }
    }
}
