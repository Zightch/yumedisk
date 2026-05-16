use std::error::Error;
use std::fmt;

#[derive(Debug, Clone, PartialEq, Eq)]
pub enum NetworkClientError {
    InvalidArgument(&'static str),
    UnauthorizedDisk { disk_id: String },
    ReadOnlySession,
    ConnectionClosed,
    Unimplemented(&'static str),
}

impl fmt::Display for NetworkClientError {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            Self::InvalidArgument(name) => write!(formatter, "invalid-argument: {}", name),
            Self::UnauthorizedDisk { disk_id } => write!(formatter, "unauthorized-disk: {}", disk_id),
            Self::ReadOnlySession => formatter.write_str("read-only-session"),
            Self::ConnectionClosed => formatter.write_str("connection-closed"),
            Self::Unimplemented(area) => write!(formatter, "unimplemented: {}", area),
        }
    }
}

impl Error for NetworkClientError {}
