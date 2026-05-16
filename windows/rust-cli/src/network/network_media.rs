use backend_rust::BackendError;
use backend_rust::Media;

use super::disk_session::DiskSession;
use super::error::NetworkClientError;

#[derive(Debug, Clone)]
pub struct NetworkMedia {
    session: DiskSession,
    disk_size_bytes: u64,
    read_only: bool,
    max_io_bytes: u32,
}

impl NetworkMedia {
    pub fn bind(
        session: DiskSession,
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
        if session.disk_size_bytes() != disk_size_bytes {
            return Err(NetworkClientError::InvalidArgument("disk_size_bytes"));
        }
        if session.read_only() != read_only {
            return Err(NetworkClientError::InvalidArgument("read_only"));
        }
        if session.max_io_bytes() != max_io_bytes {
            return Err(NetworkClientError::InvalidArgument("max_io_bytes"));
        }

        Ok(Self {
            session,
            disk_size_bytes,
            read_only,
            max_io_bytes,
        })
    }

    pub fn session(&self) -> &DiskSession {
        &self.session
    }

    pub fn read_only(&self) -> bool {
        self.read_only
    }

    pub fn max_io_bytes(&self) -> u32 {
        self.max_io_bytes
    }
}

impl Media for NetworkMedia {
    fn size_bytes(&self) -> u64 {
        self.disk_size_bytes
    }

    fn read_locked(&self, offset: u64, buffer: &mut [u8]) -> Result<(), BackendError> {
        validate_range(self.disk_size_bytes, offset, buffer.len())?;
        if buffer.len() > self.max_io_bytes as usize {
            return Err(BackendError::InvalidParameter);
        }

        self.session
            .read_at(offset, buffer)
            .map_err(map_network_error_to_backend_error)
    }

    fn write_locked(&self, offset: u64, data: &[u8]) -> Result<(), BackendError> {
        validate_range(self.disk_size_bytes, offset, data.len())?;
        if self.read_only {
            return Err(BackendError::InvalidParameter);
        }
        if data.len() > self.max_io_bytes as usize {
            return Err(BackendError::InvalidParameter);
        }

        self.session
            .write_at(offset, data)
            .map_err(map_network_error_to_backend_error)
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
        NetworkClientError::InvalidArgument(_) => BackendError::InvalidParameter,
        NetworkClientError::ReadOnlySession => BackendError::InvalidParameter,
        NetworkClientError::UnauthorizedDisk { .. }
        | NetworkClientError::ConnectionClosed
        | NetworkClientError::Unimplemented(_) => BackendError::SessionNotOpen,
    }
}

#[cfg(test)]
mod tests {
    use super::NetworkMedia;
    use crate::network::DiskSession;
    use crate::network::GatewayConnection;
    use crate::network::TransportEndpoint;

    #[test]
    fn bind_requires_session_metadata_to_match_media_metadata() {
        let connection = GatewayConnection::new(TransportEndpoint::new("127.0.0.1:9000"));
        let session = DiskSession::new(connection, "disk-1", 7, 4096, false, 1024)
            .expect("session should build");

        let error = NetworkMedia::bind(session, 2048, false, 1024).expect_err("bind should fail");
        assert_eq!(error.to_string(), "invalid-argument: disk_size_bytes");
    }
}
