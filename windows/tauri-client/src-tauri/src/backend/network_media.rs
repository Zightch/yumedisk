use std::fmt;

use backend_rust::BackendError;
use backend_rust::Media;
use network_core::client::DiskSession;
use network_core::client::NetworkClientError;
use network_core::client::SessionMetadata;

#[derive(Clone)]
pub struct NetworkMedia {
    remote_disk_id: String,
    session: DiskSession,
    disk_size_bytes: u64,
    read_only: bool,
    max_io_bytes: u32,
}

impl fmt::Debug for NetworkMedia {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        formatter
            .debug_struct("NetworkMedia")
            .field("remote_disk_id", &self.remote_disk_id)
            .field("session_id", &self.session.session_id())
            .field("disk_size_bytes", &self.disk_size_bytes)
            .field("read_only", &self.read_only)
            .field("max_io_bytes", &self.max_io_bytes)
            .finish()
    }
}

impl NetworkMedia {
    pub fn bind(
        remote_disk_id: impl Into<String>,
        session: DiskSession,
        metadata: SessionMetadata,
    ) -> Result<Self, NetworkClientError> {
        let remote_disk_id = remote_disk_id.into();
        if remote_disk_id.is_empty() {
            return Err(NetworkClientError::InvalidArgument("remote_disk_id"));
        }
        if metadata.disk_size_bytes == 0 {
            return Err(NetworkClientError::InvalidArgument("disk_size_bytes"));
        }
        if metadata.max_io_bytes == 0 {
            return Err(NetworkClientError::InvalidArgument("max_io_bytes"));
        }

        Ok(Self {
            remote_disk_id,
            session,
            disk_size_bytes: metadata.disk_size_bytes,
            read_only: metadata.read_only,
            max_io_bytes: metadata.max_io_bytes,
        })
    }
}

impl Media for NetworkMedia {
    fn size_bytes(&self) -> u64 {
        self.disk_size_bytes
    }

    fn read_locked(&self, offset: u64, buffer: &mut [u8]) -> Result<(), BackendError> {
        validate_range(self.disk_size_bytes, offset, buffer.len())?;
        if buffer.is_empty() {
            return Ok(());
        }

        let mut remaining = buffer;
        let mut chunk_offset = offset;
        while !remaining.is_empty() {
            let chunk_len = remaining.len().min(self.max_io_bytes as usize);
            let (chunk, rest) = remaining.split_at_mut(chunk_len);
            if let Err(error) = self.session.read_at(chunk_offset, chunk) {
                return Err(map_network_error_to_backend_error(error));
            }
            chunk_offset = chunk_offset
                .checked_add(u64::try_from(chunk_len).map_err(|_| BackendError::InvalidParameter)?)
                .ok_or(BackendError::InvalidParameter)?;
            remaining = rest;
        }

        Ok(())
    }

    fn write_locked(&self, offset: u64, data: &[u8]) -> Result<(), BackendError> {
        validate_range(self.disk_size_bytes, offset, data.len())?;
        if self.read_only {
            return Err(BackendError::InvalidParameter);
        }
        if data.is_empty() {
            return Ok(());
        }

        let mut remaining = data;
        let mut chunk_offset = offset;
        while !remaining.is_empty() {
            let chunk_len = remaining.len().min(self.max_io_bytes as usize);
            let (chunk, rest) = remaining.split_at(chunk_len);
            if let Err(error) = self.session.write_at(chunk_offset, chunk) {
                return Err(map_network_error_to_backend_error(error));
            }
            chunk_offset = chunk_offset
                .checked_add(u64::try_from(chunk_len).map_err(|_| BackendError::InvalidParameter)?)
                .ok_or(BackendError::InvalidParameter)?;
            remaining = rest;
        }

        Ok(())
    }
}

fn validate_range(disk_size_bytes: u64, offset: u64, length: usize) -> Result<(), BackendError> {
    let end = offset
        .checked_add(u64::try_from(length).map_err(|_| BackendError::InvalidParameter)?)
        .ok_or(BackendError::InvalidParameter)?;
    if end > disk_size_bytes {
        return Err(BackendError::InvalidParameter);
    }
    Ok(())
}

fn map_network_error_to_backend_error(error: NetworkClientError) -> BackendError {
    match error {
        NetworkClientError::InvalidArgument(_)
        | NetworkClientError::InvalidState(_)
        | NetworkClientError::OpenRejected
        | NetworkClientError::InvalidIo(_)
        | NetworkClientError::IoFailed => BackendError::InvalidParameter,
        NetworkClientError::SessionUnavailable
        | NetworkClientError::UnauthorizedDisk { .. }
        | NetworkClientError::AlreadyConnected
        | NetworkClientError::PendingRequestConflict { .. }
        | NetworkClientError::UnknownPendingRequest { .. }
        | NetworkClientError::Protocol(_)
        | NetworkClientError::Transport(_)
        | NetworkClientError::Crypto(_)
        | NetworkClientError::Unimplemented(_) => BackendError::SessionNotOpen,
    }
}
