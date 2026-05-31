use std::env;
use std::io;
use std::io::Write;

use network_core::client::AuthGrant;
use network_core::client::ConnectionAuthenticator;
use network_core::client::DiskSession;
use network_core::client::GatewayConnection;
use network_core::client::SessionDescriber;
use network_core::client::SessionMetadata;
use network_core::client::SessionOpener;
use network_core::transport::TransportEndpoint;

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
    auth: Option<AuthGrant>,
    disk_id: Option<String>,
    session: Option<DiskSession>,
    metadata: Option<SessionMetadata>,
}

impl DebugShell {
    fn new(connection: std::sync::Arc<GatewayConnection>, claim_code: String) -> Self {
        Self {
            connection,
            claim_code,
            auth: None,
            disk_id: None,
            session: None,
            metadata: None,
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
        let auth = authenticator
            .authenticate(&self.claim_code)
            .map_err(|error| error.to_string())?;
        println!(
            "auth_ok disk_id={} auth_id={}",
            auth.disk_id(),
            auth.auth_id()
        );
        self.disk_id = Some(auth.disk_id().to_string());
        self.auth = Some(auth);
        Ok(())
    }

    fn open(&mut self) -> Result<(), String> {
        let Some(auth) = self.auth.clone() else {
            return Err("auth-required".to_string());
        };
        if self.session.is_some() {
            return Err("session-already-open".to_string());
        }

        let opener = SessionOpener::new(self.connection.clone());
        let session_id = opener.open(&auth).map_err(|error| error.to_string())?;
        let session = DiskSession::new(self.connection.clone(), session_id)
            .map_err(|error| error.to_string())?;
        let describer = SessionDescriber::new(self.connection.clone());
        let metadata = match describer.describe(session_id) {
            Ok(metadata) => metadata,
            Err(error) => {
                let _ = session.close();
                return Err(error.to_string());
            }
        };
        println!(
            "open_ok disk_id={} session_id={} disk_bytes={} read_only={} raw_limit_bytes={}",
            auth.disk_id(),
            session.session_id(),
            metadata.disk_size_bytes,
            metadata.read_only,
            network_core::protocol::MAX_DATA_PLANE_RAW_BYTES
        );
        self.disk_id = Some(auth.disk_id().to_string());
        self.auth = None;
        self.metadata = Some(metadata);
        self.session = Some(session);
        Ok(())
    }

    fn close_session(&mut self) -> Result<(), String> {
        let Some(session) = self.session.take() else {
            println!("close_skip no-session");
            return Ok(());
        };

        session.close().map_err(|error| error.to_string())?;
        self.metadata = None;
        println!("closed session_id={}", session.session_id());
        Ok(())
    }

    fn print_state(&self) {
        let disk_id = self
            .auth
            .as_ref()
            .map(|auth| auth.disk_id().to_string())
            .or_else(|| self.disk_id.clone())
            .unwrap_or_else(|| "-".to_string());
        let phase = self.connection.phase_name();

        match &self.session {
            Some(session) => {
                println!(
                    "state connected={} phase={} disk_id={} session=open({}) terminal={} closed={}",
                    bool_to_text(self.connection.is_connected()),
                    phase,
                    disk_id,
                    session.session_id(),
                    bool_to_text(session.is_terminal()),
                    bool_to_text(session.is_closed())
                );
            }
            None => {
                println!(
                    "state connected={} phase={} disk_id={} session=none",
                    bool_to_text(self.connection.is_connected()),
                    phase,
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
        println!("  open   run SessionOpen/SessionDescribe using current auth grant");
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
