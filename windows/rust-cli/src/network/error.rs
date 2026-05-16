use std::error::Error;
use std::fmt;

use super::protocol_client::ProtocolClientError;

#[derive(Debug, Clone, PartialEq, Eq)]
pub enum NetworkClientError {
    InvalidArgument(&'static str),
    UnauthorizedDisk { disk_id: String },
    ReadOnlySession,
    ConnectionClosed,
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
            Self::UnauthorizedDisk { disk_id } => write!(formatter, "unauthorized-disk: {}", disk_id),
            Self::ReadOnlySession => formatter.write_str("read-only-session"),
            Self::ConnectionClosed => formatter.write_str("connection-closed"),
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
