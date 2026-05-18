use std::error::Error;
use std::fmt;

use super::protocol_client::ProtocolClientError;

#[derive(Debug, Clone, PartialEq, Eq)]
pub enum NetworkClientError {
    InvalidArgument(&'static str),
    InvalidState(&'static str),
    UnauthorizedDisk { disk_id: String },
    InvalidIo(&'static str),
    IoFailed,
    SessionUnavailable,
    AlreadyConnected,
    PendingRequestConflict { request_id: u64 },
    UnknownPendingRequest { request_id: u64 },
    Protocol(ProtocolClientError),
    Transport(String),
    Crypto(String),
    Unimplemented(&'static str),
}

impl fmt::Display for NetworkClientError {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            Self::InvalidArgument(name) => write!(formatter, "invalid-argument: {}", name),
            Self::InvalidState(name) => write!(formatter, "invalid-state: {}", name),
            Self::UnauthorizedDisk { disk_id } => {
                write!(formatter, "unauthorized-disk: {}", disk_id)
            }
            Self::InvalidIo(reason) => write!(formatter, "invalid-io: {}", reason),
            Self::IoFailed => formatter.write_str("io-failed"),
            Self::SessionUnavailable => formatter.write_str("session-unavailable"),
            Self::AlreadyConnected => formatter.write_str("already-connected"),
            Self::PendingRequestConflict { request_id } => {
                write!(formatter, "pending-request-conflict: {}", request_id)
            }
            Self::UnknownPendingRequest { request_id } => {
                write!(formatter, "unknown-pending-request: {}", request_id)
            }
            Self::Protocol(error) => write!(formatter, "protocol: {}", error),
            Self::Transport(message) => write!(formatter, "transport: {}", message),
            Self::Crypto(message) => write!(formatter, "crypto: {}", message),
            Self::Unimplemented(area) => write!(formatter, "unimplemented: {}", area),
        }
    }
}

impl Error for NetworkClientError {}
