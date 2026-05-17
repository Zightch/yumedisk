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
        if buffer.is_empty() {
            return Ok(());
        }

        let mut remaining = buffer;
        let mut chunk_offset = offset;
        while !remaining.is_empty() {
            let chunk_len = remaining.len().min(self.max_io_bytes as usize);
            let (chunk, rest) = remaining.split_at_mut(chunk_len);
            self.session
                .read_at(chunk_offset, chunk)
                .map_err(map_network_error_to_backend_error)?;
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
            self.session
                .write_at(chunk_offset, chunk)
                .map_err(map_network_error_to_backend_error)?;
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
    use crate::network::TransportEndpoint;
    use crate::network::transport_client::MAX_FRAME_PAYLOAD_BYTES;
    use crate::network::transport_client::read_frame_into;
    use crate::network::transport_client::write_frame;
    use backend_rust::BackendError;
    use backend_rust::Media;
    use std::net::TcpListener;
    use std::thread;

    #[test]
    fn bind_requires_session_metadata_to_match_media_metadata() {
        let connection = GatewayConnection::new(TransportEndpoint::new("127.0.0.1:9000"));
        let session = DiskSession::new(connection, "disk-1", 7, 4096, false, 1024, 300)
            .expect("session should build");

        let error = NetworkMedia::bind(session, 2048, false, 1024).expect_err("bind should fail");
        assert_eq!(error.to_string(), "invalid-argument: disk_size_bytes");
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

        let connection = GatewayConnection::new(TransportEndpoint::new(address.to_string()));
        connection.connect().expect("connect should succeed");
        let session = DiskSession::new(
            connection.clone(),
            "A1b2C3d4E5f6G7h8",
            77,
            4096,
            false,
            4,
            300,
        )
        .expect("session should build");
        let media = NetworkMedia::bind(session, 4096, false, 4).expect("bind should succeed");

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

        let connection = GatewayConnection::new(TransportEndpoint::new(address.to_string()));
        connection.connect().expect("connect should succeed");
        let session = DiskSession::new(
            connection.clone(),
            "A1b2C3d4E5f6G7h8",
            77,
            4096,
            false,
            4,
            300,
        )
        .expect("session should build");
        let media = NetworkMedia::bind(session, 4096, false, 4).expect("bind should succeed");

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
}
