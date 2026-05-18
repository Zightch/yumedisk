use backend_rust::BackendError;
use backend_rust::Media;
use std::fmt;
use std::sync::Arc;

use super::disk_session::DiskSession;
use super::error::NetworkClientError;
use super::session_describer::SessionMetadata;

#[derive(Clone)]
pub struct NetworkMedia {
    disk_id: String,
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
            .field("disk_id", &self.disk_id)
            .field("session_id", &self.session.session_id())
            .field("disk_size_bytes", &self.disk_size_bytes)
            .field("read_only", &self.read_only)
            .field("max_io_bytes", &self.max_io_bytes)
            .finish()
    }
}

impl NetworkMedia {
    pub fn bind(
        disk_id: impl Into<String>,
        session: DiskSession,
        metadata: SessionMetadata,
    ) -> Result<Self, NetworkClientError> {
        let disk_id = disk_id.into();
        if disk_id.is_empty() {
            return Err(NetworkClientError::InvalidArgument("disk_id"));
        }
        if metadata.disk_size_bytes == 0 {
            return Err(NetworkClientError::InvalidArgument("disk_size_bytes"));
        }
        if metadata.max_io_bytes == 0 {
            return Err(NetworkClientError::InvalidArgument("max_io_bytes"));
        }

        Ok(Self {
            disk_id,
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

    pub fn disk_id(&self) -> &str {
        self.disk_id.as_str()
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
        | NetworkClientError::InvalidIo(_)
        | NetworkClientError::IoFailed => BackendError::InvalidParameter,
        NetworkClientError::SessionUnavailable
        | NetworkClientError::DiskBusy { .. }
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
    use super::map_network_error_to_backend_error;
    use crate::network::ClientOperationCode;
    use crate::network::DiskSession;
    use crate::network::FLAG_RESPONSE;
    use crate::network::GatewayConnection;
    use crate::network::HEADER_SIZE;
    use crate::network::NetworkClientError;
    use crate::network::PROTOCOL_VERSION;
    use crate::network::ProtocolHeader;
    use crate::network::ProtocolStatusCode;
    use crate::network::SessionMetadata;
    use crate::network::TransportEndpoint;
    use crate::network::transport_client::MAX_FRAME_PAYLOAD_BYTES;
    use crate::network::transport_client::read_frame_into;
    use crate::network::transport_client::write_frame;
    use backend_rust::BackendError;
    use backend_rust::Media;
    use std::net::TcpListener;
    use std::sync::Arc;
    use std::sync::mpsc;
    use std::thread;
    use std::time::Duration;

    fn staged_connection(
        endpoint: TransportEndpoint,
        disk_id: &str,
        session_id: u64,
    ) -> Arc<GatewayConnection> {
        let connection = GatewayConnection::new(endpoint);
        connection
            .begin_auth(disk_id)
            .expect("begin auth should succeed");
        connection
            .finish_auth(disk_id, 9)
            .expect("finish auth should succeed");
        connection
            .begin_session_open(disk_id, 9)
            .expect("begin session open should succeed");
        connection
            .finish_session_open(disk_id, 9, session_id)
            .expect("finish session open should succeed");
        connection
    }

    #[test]
    fn bind_requires_explicit_disk_id_and_metadata() {
        let connection = staged_connection(TransportEndpoint::new("127.0.0.1:9000"), "disk-1", 7);
        let session = DiskSession::new(connection, 7).expect("session should build");

        let error = NetworkMedia::bind(
            "",
            session,
            SessionMetadata {
                disk_size_bytes: 2048,
                read_only: false,
                max_io_bytes: 1024,
            },
        )
        .expect_err("bind should fail");
        assert_eq!(error.to_string(), "invalid-argument: disk_id");
    }

    #[test]
    fn read_locked_splits_large_requests_by_max_io_bytes() {
        let listener = TcpListener::bind("127.0.0.1:0").expect("bind should succeed");
        let address = listener.local_addr().expect("local addr should succeed");

        let server = thread::spawn(move || {
            let (mut stream, _) = listener.accept().expect("accept should succeed");
            let mut buffer = vec![0u8; MAX_FRAME_PAYLOAD_BYTES];

            let first = read_frame_into(&mut stream, &mut buffer)
                .expect("read first request should succeed")
                .to_vec();
            let first_header = crate::network::parse_request_header(&first).expect("parse first");
            assert_eq!(first_header.op_code, ClientOperationCode::ReadAt);
            assert_eq!(&first[HEADER_SIZE..HEADER_SIZE + 8], &0u64.to_be_bytes());
            assert_eq!(
                &first[HEADER_SIZE + 8..HEADER_SIZE + 12],
                &4u32.to_be_bytes()
            );
            let first_response = ProtocolHeader {
                protocol_version: PROTOCOL_VERSION,
                header_len: HEADER_SIZE as u8,
                op_code: ClientOperationCode::ReadAt,
                flags: FLAG_RESPONSE,
                status_code: ProtocolStatusCode::Ok,
                reserved: 0,
                request_id: first_header.request_id,
                session_id: first_header.session_id,
            }
            .encode(b"ABCD");
            write_frame(&mut stream, &first_response).expect("write first response");

            let second = read_frame_into(&mut stream, &mut buffer)
                .expect("read second request should succeed")
                .to_vec();
            let second_header =
                crate::network::parse_request_header(&second).expect("parse second");
            assert_eq!(second_header.op_code, ClientOperationCode::ReadAt);
            assert_eq!(&second[HEADER_SIZE..HEADER_SIZE + 8], &4u64.to_be_bytes());
            assert_eq!(
                &second[HEADER_SIZE + 8..HEADER_SIZE + 12],
                &4u32.to_be_bytes()
            );
            let second_response = ProtocolHeader {
                protocol_version: PROTOCOL_VERSION,
                header_len: HEADER_SIZE as u8,
                op_code: ClientOperationCode::ReadAt,
                flags: FLAG_RESPONSE,
                status_code: ProtocolStatusCode::Ok,
                reserved: 0,
                request_id: second_header.request_id,
                session_id: second_header.session_id,
            }
            .encode(b"EFGH");
            write_frame(&mut stream, &second_response).expect("write second response");
        });

        let connection = staged_connection(
            TransportEndpoint::new(address.to_string()),
            "A1b2C3d4E5f6G7h8",
            77,
        );
        connection.connect().expect("connect should succeed");
        let session = DiskSession::new(connection.clone(), 77).expect("session should build");
        let media = NetworkMedia::bind(
            "A1b2C3d4E5f6G7h8",
            session,
            SessionMetadata {
                disk_size_bytes: 4096,
                read_only: false,
                max_io_bytes: 4,
            },
        )
        .expect("bind should succeed");
        assert_eq!(media.disk_id(), "A1b2C3d4E5f6G7h8");

        let mut buffer = [0u8; 8];
        media
            .read_locked(0, &mut buffer)
            .expect("read should succeed");
        assert_eq!(&buffer, b"ABCDEFGH");

        connection.close().expect("close should succeed");
        server.join().expect("server should join");
    }

    #[test]
    fn write_locked_splits_large_requests_by_max_io_bytes() {
        let listener = TcpListener::bind("127.0.0.1:0").expect("bind should succeed");
        let address = listener.local_addr().expect("local addr should succeed");

        let server = thread::spawn(move || {
            let (mut stream, _) = listener.accept().expect("accept should succeed");
            let mut buffer = vec![0u8; MAX_FRAME_PAYLOAD_BYTES];

            let first = read_frame_into(&mut stream, &mut buffer)
                .expect("read first request should succeed")
                .to_vec();
            let first_header = crate::network::parse_request_header(&first).expect("parse first");
            assert_eq!(first_header.op_code, ClientOperationCode::WriteAt);
            assert_eq!(&first[HEADER_SIZE..HEADER_SIZE + 8], &0u64.to_be_bytes());
            assert_eq!(
                &first[HEADER_SIZE + 8..HEADER_SIZE + 12],
                &4u32.to_be_bytes()
            );
            assert_eq!(&first[HEADER_SIZE + 12..], b"ABCD");
            let first_response = ProtocolHeader {
                protocol_version: PROTOCOL_VERSION,
                header_len: HEADER_SIZE as u8,
                op_code: ClientOperationCode::WriteAt,
                flags: FLAG_RESPONSE,
                status_code: ProtocolStatusCode::Ok,
                reserved: 0,
                request_id: first_header.request_id,
                session_id: first_header.session_id,
            }
            .encode(&[]);
            write_frame(&mut stream, &first_response).expect("write first response");

            let second = read_frame_into(&mut stream, &mut buffer)
                .expect("read second request should succeed")
                .to_vec();
            let second_header =
                crate::network::parse_request_header(&second).expect("parse second");
            assert_eq!(second_header.op_code, ClientOperationCode::WriteAt);
            assert_eq!(&second[HEADER_SIZE..HEADER_SIZE + 8], &4u64.to_be_bytes());
            assert_eq!(
                &second[HEADER_SIZE + 8..HEADER_SIZE + 12],
                &4u32.to_be_bytes()
            );
            assert_eq!(&second[HEADER_SIZE + 12..], b"EFGH");
            let second_response = ProtocolHeader {
                protocol_version: PROTOCOL_VERSION,
                header_len: HEADER_SIZE as u8,
                op_code: ClientOperationCode::WriteAt,
                flags: FLAG_RESPONSE,
                status_code: ProtocolStatusCode::Ok,
                reserved: 0,
                request_id: second_header.request_id,
                session_id: second_header.session_id,
            }
            .encode(&[]);
            write_frame(&mut stream, &second_response).expect("write second response");
        });

        let connection = staged_connection(
            TransportEndpoint::new(address.to_string()),
            "A1b2C3d4E5f6G7h8",
            77,
        );
        connection.connect().expect("connect should succeed");
        let session = DiskSession::new(connection.clone(), 77).expect("session should build");
        let media = NetworkMedia::bind(
            "A1b2C3d4E5f6G7h8",
            session,
            SessionMetadata {
                disk_size_bytes: 4096,
                read_only: false,
                max_io_bytes: 4,
            },
        )
        .expect("bind should succeed");

        media
            .write_locked(0, b"ABCDEFGH")
            .expect("write should succeed");

        connection.close().expect("close should succeed");
        server.join().expect("server should join");
    }

    #[test]
    fn network_media_maps_network_failures_to_backend_errors() {
        assert_eq!(
            map_network_error_to_backend_error(NetworkClientError::SessionUnavailable),
            BackendError::SessionNotOpen
        );
        assert_eq!(
            map_network_error_to_backend_error(NetworkClientError::Transport(
                "disconnect".to_string()
            )),
            BackendError::SessionNotOpen
        );
        assert_eq!(
            map_network_error_to_backend_error(NetworkClientError::IoFailed),
            BackendError::InvalidParameter
        );
        assert_eq!(
            map_network_error_to_backend_error(NetworkClientError::InvalidIo("read_only")),
            BackendError::InvalidParameter
        );
    }

    #[test]
    fn network_media_calls_invalidation_handler_on_session_loss() {
        let listener = TcpListener::bind("127.0.0.1:0").expect("bind should succeed");
        let address = listener.local_addr().expect("local addr should succeed");

        let server = thread::spawn(move || {
            let (mut stream, _) = listener.accept().expect("accept should succeed");
            let mut buffer = vec![0u8; MAX_FRAME_PAYLOAD_BYTES];
            let request = read_frame_into(&mut stream, &mut buffer)
                .expect("read request should succeed")
                .to_vec();
            let header = crate::network::parse_request_header(&request).expect("parse request");
            assert_eq!(header.op_code, ClientOperationCode::ReadAt);
            let response = ProtocolHeader {
                protocol_version: PROTOCOL_VERSION,
                header_len: HEADER_SIZE as u8,
                op_code: ClientOperationCode::ReadAt,
                flags: FLAG_RESPONSE,
                status_code: ProtocolStatusCode::ErrSessionUnavailable,
                reserved: 0,
                request_id: header.request_id,
                session_id: header.session_id,
            }
            .encode(&[]);
            write_frame(&mut stream, &response).expect("write response");
            thread::sleep(Duration::from_millis(20));
        });

        let connection = staged_connection(
            TransportEndpoint::new(address.to_string()),
            "A1b2C3d4E5f6G7h8",
            77,
        );
        connection.connect().expect("connect should succeed");
        let session = DiskSession::new(connection.clone(), 77).expect("session should build");
        let (invalidate_tx, invalidate_rx) = mpsc::channel();
        let media = NetworkMedia::bind(
            "A1b2C3d4E5f6G7h8",
            session,
            SessionMetadata {
                disk_size_bytes: 4096,
                read_only: false,
                max_io_bytes: 4,
            },
        )
        .expect("bind should succeed")
        .with_invalidation_handler(Arc::new(move || {
            let _ = invalidate_tx.send(());
        }));

        let mut buffer = [0u8; 4];
        let error = media
            .read_locked(0, &mut buffer)
            .expect_err("read should fail");
        assert_eq!(error, BackendError::SessionNotOpen);
        invalidate_rx
            .recv_timeout(Duration::from_millis(200))
            .expect("invalidation handler should fire");

        connection.close().expect("close should succeed");
        server.join().expect("server should join");
    }
}
