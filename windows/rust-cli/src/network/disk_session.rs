use std::sync::Arc;
use std::time::SystemTime;

use super::error::NetworkClientError;
use super::gateway_connection::GatewayConnection;

#[derive(Debug, Clone)]
pub struct DiskSession {
    connection: Arc<GatewayConnection>,
    disk_id: String,
    session_id: u64,
    disk_size_bytes: u64,
    read_only: bool,
    max_io_bytes: u32,
    opened_at: SystemTime,
}

impl DiskSession {
    pub fn new(
        connection: Arc<GatewayConnection>,
        disk_id: impl Into<String>,
        session_id: u64,
        disk_size_bytes: u64,
        read_only: bool,
        max_io_bytes: u32,
    ) -> Result<Self, NetworkClientError> {
        if disk_size_bytes == 0 {
            return Err(NetworkClientError::InvalidArgument("disk_size_bytes"));
        }
        if max_io_bytes == 0 {
            return Err(NetworkClientError::InvalidArgument("max_io_bytes"));
        }

        Ok(Self {
            connection,
            disk_id: disk_id.into(),
            session_id,
            disk_size_bytes,
            read_only,
            max_io_bytes,
            opened_at: SystemTime::now(),
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
        self.opened_at
    }

    pub fn read_at(&self, _offset: u64, _buffer: &mut [u8]) -> Result<(), NetworkClientError> {
        Err(NetworkClientError::Unimplemented(
            "B6 disk session read path will be implemented later",
        ))
    }

    pub fn write_at(&self, _offset: u64, _data: &[u8]) -> Result<(), NetworkClientError> {
        if self.read_only {
            return Err(NetworkClientError::ReadOnlySession);
        }

        Err(NetworkClientError::Unimplemented(
            "B6 disk session write path will be implemented later",
        ))
    }
}
