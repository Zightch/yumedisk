use std::sync::Arc;
use std::sync::atomic::AtomicU8;
use std::sync::atomic::Ordering;
use std::time::Duration;
use std::time::SystemTime;

use super::error::NetworkClientError;
use super::gateway_connection::GatewayConnection;

const STATE_OPEN: u8 = 0;
const STATE_CLOSED: u8 = 1;

#[derive(Debug, Clone)]
pub struct DiskSession {
    connection: Arc<GatewayConnection>,
    disk_id: String,
    session_id: u64,
    disk_size_bytes: u64,
    read_only: bool,
    max_io_bytes: u32,
    lifecycle: Arc<DiskSessionLifecycle>,
}

#[derive(Debug)]
struct DiskSessionLifecycle {
    opened_at: SystemTime,
    expires_at: SystemTime,
    session_ttl_seconds: u32,
    state: AtomicU8,
}

impl DiskSession {
    pub fn new(
        connection: Arc<GatewayConnection>,
        disk_id: impl Into<String>,
        session_id: u64,
        disk_size_bytes: u64,
        read_only: bool,
        max_io_bytes: u32,
        session_ttl_seconds: u32,
    ) -> Result<Self, NetworkClientError> {
        if session_id == 0 {
            return Err(NetworkClientError::InvalidArgument("session_id"));
        }
        if disk_size_bytes == 0 {
            return Err(NetworkClientError::InvalidArgument("disk_size_bytes"));
        }
        if max_io_bytes == 0 {
            return Err(NetworkClientError::InvalidArgument("max_io_bytes"));
        }
        if session_ttl_seconds == 0 {
            return Err(NetworkClientError::InvalidArgument("session_ttl_seconds"));
        }

        let opened_at = SystemTime::now();
        let expires_at = opened_at
            .checked_add(Duration::from_secs(u64::from(session_ttl_seconds)))
            .ok_or(NetworkClientError::InvalidArgument("session_ttl_seconds"))?;

        Ok(Self {
            connection,
            disk_id: disk_id.into(),
            session_id,
            disk_size_bytes,
            read_only,
            max_io_bytes,
            lifecycle: Arc::new(DiskSessionLifecycle {
                opened_at,
                expires_at,
                session_ttl_seconds,
                state: AtomicU8::new(STATE_OPEN),
            }),
        })
    }

    pub fn connection(&self) -> &Arc<GatewayConnection> {
        &self.connection
    }

    pub fn disk_id(&self) -> &str {
        self.disk_id.as_str()
    }

    pub fn session_id(&self) -> u64 {
        self.session_id
    }

    pub fn disk_size_bytes(&self) -> u64 {
        self.disk_size_bytes
    }

    pub fn read_only(&self) -> bool {
        self.read_only
    }

    pub fn max_io_bytes(&self) -> u32 {
        self.max_io_bytes
    }

    pub fn opened_at(&self) -> SystemTime {
        self.lifecycle.opened_at
    }

    pub fn expires_at(&self) -> SystemTime {
        self.lifecycle.expires_at
    }

    pub fn session_ttl_seconds(&self) -> u32 {
        self.lifecycle.session_ttl_seconds
    }

    pub fn is_closed(&self) -> bool {
        self.lifecycle.state.load(Ordering::Acquire) == STATE_CLOSED
    }

    pub fn is_expired(&self) -> bool {
        SystemTime::now() >= self.lifecycle.expires_at
    }

    pub fn is_connection_alive(&self) -> bool {
        self.connection.is_connected()
    }

    pub fn mark_closed(&self) {
        self.lifecycle.state.store(STATE_CLOSED, Ordering::Release);
    }

    pub fn ensure_usable(&self) -> Result<(), NetworkClientError> {
        if self.is_closed() {
            return Err(NetworkClientError::SessionClosed);
        }
        if self.is_expired() {
            return Err(NetworkClientError::SessionExpired);
        }
        if !self.is_connection_alive() {
            return Err(NetworkClientError::ConnectionClosed);
        }
        Ok(())
    }

    pub fn read_at(&self, _offset: u64, _buffer: &mut [u8]) -> Result<(), NetworkClientError> {
        self.ensure_usable()?;
        Err(NetworkClientError::Unimplemented(
            "B6 disk session read path will be implemented later",
        ))
    }

    pub fn write_at(&self, _offset: u64, _data: &[u8]) -> Result<(), NetworkClientError> {
        self.ensure_usable()?;
        if self.read_only {
            return Err(NetworkClientError::ReadOnlySession);
        }

        Err(NetworkClientError::Unimplemented(
            "B6 disk session write path will be implemented later",
        ))
    }
}

#[cfg(test)]
mod tests {
    use super::DiskSession;
    use crate::network::GatewayConnection;
    use crate::network::TransportEndpoint;

    #[test]
    fn disk_session_saves_session_metadata_and_lifecycle() {
        let connection = GatewayConnection::new(TransportEndpoint::new("127.0.0.1:1"));
        let session = DiskSession::new(
            connection,
            "A1b2C3d4E5f6G7h8",
            77,
            4096,
            true,
            60_000,
            300,
        )
        .expect("session should build");

        assert_eq!(session.disk_id(), "A1b2C3d4E5f6G7h8");
        assert_eq!(session.session_id(), 77);
        assert_eq!(session.disk_size_bytes(), 4096);
        assert_eq!(session.max_io_bytes(), 60_000);
        assert!(session.read_only());
        assert_eq!(session.session_ttl_seconds(), 300);
        assert!(session.expires_at() > session.opened_at());
        assert!(!session.is_closed());
    }

    #[test]
    fn disk_session_shares_closed_state_across_clones() {
        let connection = GatewayConnection::new(TransportEndpoint::new("127.0.0.1:1"));
        let session = DiskSession::new(
            connection,
            "A1b2C3d4E5f6G7h8",
            77,
            4096,
            false,
            60_000,
            300,
        )
        .expect("session should build");
        let cloned = session.clone();

        cloned.mark_closed();

        assert!(session.is_closed());
        assert_eq!(
            session.ensure_usable().expect_err("closed session should fail").to_string(),
            "session-closed"
        );
    }

    #[test]
    fn disk_session_rejects_invalid_lifecycle_inputs() {
        let connection = GatewayConnection::new(TransportEndpoint::new("127.0.0.1:1"));

        let error = DiskSession::new(connection, "A1b2C3d4E5f6G7h8", 0, 4096, false, 60_000, 300)
            .expect_err("zero session id should fail");
        assert_eq!(error.to_string(), "invalid-argument: session_id");
    }
}
