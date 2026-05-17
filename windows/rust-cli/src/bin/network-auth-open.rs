use std::env;
use std::io;
use std::io::Write;

use rust_cli::network::ConnectionAuthenticator;
use rust_cli::network::DiskSession;
use rust_cli::network::GatewayConnection;
use rust_cli::network::SessionOpener;
use rust_cli::network::TransportEndpoint;

fn main() {
    if let Err(error) = run() {
        eprintln!("error: {}", error);
        std::process::exit(1);
    }
}

fn run() -> Result<(), String> {
    let args = env::args().collect::<Vec<_>>();
    if args.len() != 3 {
        return Err("usage: network-auth-open <addr> <claim_code>".to_string());
    }

    let addr = args[1].clone();
    let claim_code = args[2].clone();

    let connection = GatewayConnection::new(TransportEndpoint::new(addr.clone()));
    connection.connect().map_err(|error| error.to_string())?;

    let mut shell = DebugShell::new(connection, claim_code);
    println!("state=ready(network-auth-open)");
    shell.print_state();
    shell.print_help();
    shell.run_loop()
}

struct DebugShell {
    connection: std::sync::Arc<GatewayConnection>,
    claim_code: String,
    disk_id: Option<String>,
    session: Option<DiskSession>,
}

impl DebugShell {
    fn new(connection: std::sync::Arc<GatewayConnection>, claim_code: String) -> Self {
        Self {
            connection,
            claim_code,
            disk_id: None,
            session: None,
        }
    }

    fn run_loop(&mut self) -> Result<(), String> {
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
                self.shutdown();
                println!("state=eof");
                return Ok(());
            }

            let tokens = line.split_whitespace().collect::<Vec<_>>();
            if tokens.is_empty() {
                continue;
            }

            match tokens[0].to_ascii_lowercase().as_str() {
                "help" => self.print_help(),
                "state" => self.print_state(),
                "auth" => {
                    if let Err(error) = self.auth() {
                        eprintln!("error: {}", error);
                    }
                }
                "open" => {
                    if let Err(error) = self.open() {
                        eprintln!("error: {}", error);
                    }
                }
                "close" => {
                    if let Err(error) = self.close_session() {
                        eprintln!("error: {}", error);
                    }
                }
                "exit" | "quit" => {
                    self.shutdown();
                    return Ok(());
                }
                other => println!("unknown command: {}", other),
            }
        }
    }

    fn auth(&mut self) -> Result<(), String> {
        let authenticator = ConnectionAuthenticator::new(self.connection.clone());
        let disk_id = authenticator
            .authenticate(&self.claim_code)
            .map_err(|error| error.to_string())?;
        println!("auth_ok disk_id={}", disk_id);
        self.disk_id = Some(disk_id);
        Ok(())
    }

    fn open(&mut self) -> Result<(), String> {
        let Some(disk_id) = self.disk_id.clone() else {
            return Err("auth-required".to_string());
        };
        if self.session.is_some() {
            return Err("session-already-open".to_string());
        }

        let opener = SessionOpener::new(self.connection.clone());
        let session = opener.open(disk_id.clone()).map_err(|error| error.to_string())?;
        println!(
            "open_ok disk_id={} session_id={} disk_bytes={} read_only={} max_io_bytes={}",
            session.disk_id(),
            session.session_id(),
            session.disk_size_bytes(),
            session.read_only(),
            session.max_io_bytes()
        );
        self.session = Some(session);
        Ok(())
    }

    fn close_session(&mut self) -> Result<(), String> {
        let Some(session) = self.session.take() else {
            println!("close_skip no-session");
            return Ok(());
        };

        session.close().map_err(|error| error.to_string())?;
        println!("closed session_id={}", session.session_id());
        Ok(())
    }

    fn print_state(&self) {
        let disk_id = self.disk_id.as_deref().unwrap_or("-");
        let authorized = if self
            .disk_id
            .as_deref()
            .map(|value| self.connection.is_authorized(value))
            .unwrap_or(false)
        {
            "yes"
        } else {
            "no"
        };

        match &self.session {
            Some(session) => {
                println!(
                    "state connected={} authorized={} disk_id={} session=open({}) terminal={} closed={}",
                    bool_to_text(self.connection.is_connected()),
                    authorized,
                    disk_id,
                    session.session_id(),
                    bool_to_text(session.is_terminal()),
                    bool_to_text(session.is_closed())
                );
            }
            None => {
                println!(
                    "state connected={} authorized={} disk_id={} session=none",
                    bool_to_text(self.connection.is_connected()),
                    authorized,
                    disk_id
                );
            }
        }
    }

    fn print_help(&self) {
        println!("commands:");
        println!("  help   show this help");
        println!("  state  print connection/auth/session state");
        println!("  auth   run AuthStart/AuthFinish on current connection");
        println!("  open   run SessionOpen on current authorized connection");
        println!("  close  run Close for current session");
        println!("  exit   close session/connection and quit");
    }

    fn shutdown(&mut self) {
        if let Some(session) = self.session.take() {
            let _ = session.close();
        }
        let _ = self.connection.close();
    }
}

fn bool_to_text(value: bool) -> &'static str {
    if value { "yes" } else { "no" }
}
