use std::env;
use std::io;

use rust_cli::network::ConnectionAuthenticator;
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
    if args.len() != 4 {
        return Err("usage: network-auth-open <auth-open|auth-open-hold> <addr> <claim_code>".to_string());
    }

    let mode = args[1].as_str();
    let addr = args[2].as_str();
    let claim_code = args[3].as_str();
    let hold = match mode {
        "auth-open" => false,
        "auth-open-hold" => true,
        _ => {
            return Err(
                "usage: network-auth-open <auth-open|auth-open-hold> <addr> <claim_code>"
                    .to_string(),
            );
        }
    };

    let connection = GatewayConnection::new(TransportEndpoint::new(addr.to_string()));
    connection.connect().map_err(|error| error.to_string())?;

    let authenticator = ConnectionAuthenticator::new(connection.clone());
    let disk_id = authenticator
        .authenticate(claim_code)
        .map_err(|error| error.to_string())?;
    println!("auth_ok disk_id={}", disk_id);

    let opener = SessionOpener::new(connection.clone());
    let session = opener.open(disk_id.clone()).map_err(|error| error.to_string())?;
    println!(
        "open_ok disk_id={} session_id={} disk_bytes={} read_only={} max_io_bytes={}",
        session.disk_id(),
        session.session_id(),
        session.disk_size_bytes(),
        session.read_only(),
        session.max_io_bytes()
    );

    if !hold {
        session.close().map_err(|error| error.to_string())?;
        connection.close().map_err(|error| error.to_string())?;
        println!("closed session_id={}", session.session_id());
        return Ok(());
    }

    println!("holding session_id={} press-enter-to-close", session.session_id());
    let mut line = String::new();
    io::stdin()
        .read_line(&mut line)
        .map_err(|error| format!("stdin-read-failed: {}", error))?;

    session.close().map_err(|error| error.to_string())?;
    connection.close().map_err(|error| error.to_string())?;
    println!("closed session_id={}", session.session_id());
    Ok(())
}
