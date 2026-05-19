use std::fmt;
use std::sync::Arc;

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
    invalidation_handler: Option<Arc<dyn Fn() + Send + Sync>>,
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
            invalidation_handler: None,
        })
    }

    pub fn with_invalidation_handler(mut self, handler: Arc<dyn Fn() + Send + Sync>) -> Self {
        self.invalidation_handler = Some(handler);
        self
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
                self.handle_terminal_error(&error);
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
                self.handle_terminal_error(&error);
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

impl NetworkMedia {
    fn handle_terminal_error(&self, error: &NetworkClientError) {
        if is_terminal_media_error(error) {
            if let Some(handler) = &self.invalidation_handler {
                handler();
            }
        }
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

fn is_terminal_media_error(error: &NetworkClientError) -> bool {
    matches!(
        error,
        NetworkClientError::SessionUnavailable
            | NetworkClientError::Transport(_)
            | NetworkClientError::Protocol(_)
    )
}

#[cfg(test)]
mod tests {
    use super::NetworkMedia;
    use backend_rust::BackendError;
    use backend_rust::Media;
    use network_core::client::DiskSession;
    use network_core::client::SessionMetadata;
    use network_core::test_support::clear_session;
    use network_core::test_support::stage_connection;
    use network_core::transport::TransportEndpoint;
    use std::sync::atomic::AtomicUsize;
    use std::sync::atomic::Ordering;
    use std::sync::Arc;

    #[test]
    fn read_locked_marks_media_invalid_when_session_is_terminal() {
        let connection = stage_connection(TransportEndpoint::new("127.0.0.1:1"), 17);
        let session = DiskSession::new(connection.clone(), 17).expect("session should build");
        clear_session(&connection, 17);

        let invalidations = Arc::new(AtomicUsize::new(0));
        let media = NetworkMedia::bind(
            "A1b2C3d4E5f6G7h8",
            session,
            SessionMetadata {
                disk_size_bytes: 4096,
                read_only: false,
                max_io_bytes: 4096,
            },
        )
        .expect("bind should succeed")
        .with_invalidation_handler({
            let invalidations = Arc::clone(&invalidations);
            Arc::new(move || {
                invalidations.fetch_add(1, Ordering::SeqCst);
            })
        });

        let mut buffer = [0u8; 8];
        let error = media
            .read_locked(0, &mut buffer)
            .expect_err("read should fail");
        assert_eq!(error, BackendError::SessionNotOpen);
        assert_eq!(invalidations.load(Ordering::SeqCst), 1);
    }
}
