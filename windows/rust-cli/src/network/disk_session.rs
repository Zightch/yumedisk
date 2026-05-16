use std::sync::Arc;
use std::sync::atomic::AtomicU8;
use std::sync::atomic::Ordering;
use std::time::Duration;
use std::time::SystemTime;

use super::error::NetworkClientError;
use super::protocol_client::CloseRequest;
use super::gateway_connection::GatewayConnection;
use super::protocol_client::PingRequest;
use super::protocol_client::PingResponse;
use super::protocol_client::ProtocolClientError;
use super::protocol_client::ProtocolStatusCode;
use super::protocol_client::ReadAtRequest;
use super::protocol_client::ReadAtResponse;
use super::protocol_client::WriteAtRequest;

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

    pub fn ping(&self, nonce: u64) -> Result<u64, NetworkClientError> {
        self.ensure_usable()?;

        let request_id = self.connection.allocate_request_id();
        let payload = PingRequest {
            session_id: self.session_id,
            nonce,
        }
        .encode_request(request_id)
        .map_err(NetworkClientError::Protocol)?;

        let response_payload = self.connection.send_request_and_wait(payload)?;
        let response =
            PingResponse::decode_response(&response_payload, request_id, self.session_id)
                .map_err(map_data_plane_error)?;
        Ok(response.nonce)
    }

    pub fn close(&self) -> Result<(), NetworkClientError> {
        if self.is_closed() {
            return Ok(());
        }

        self.ensure_usable()?;

        let request_id = self.connection.allocate_request_id();
        let payload = CloseRequest {
            session_id: self.session_id,
        }
        .encode_request(request_id)
        .map_err(NetworkClientError::Protocol)?;

        let response_payload = self.connection.send_request_and_wait(payload)?;
        let close_result = CloseRequest::decode_response(&response_payload, request_id, self.session_id)
            .map_err(map_data_plane_error);
        self.mark_closed();
        close_result
    }

    pub fn read_at(&self, offset: u64, buffer: &mut [u8]) -> Result<(), NetworkClientError> {
        self.ensure_usable()?;
        validate_single_io(
            self.disk_size_bytes,
            self.max_io_bytes,
            offset,
            buffer.len(),
        )?;
        if buffer.is_empty() {
            return Ok(());
        }

        let request_id = self.connection.allocate_request_id();
        let payload = ReadAtRequest {
            session_id: self.session_id,
            offset,
            length: u32::try_from(buffer.len())
                .map_err(|_| NetworkClientError::InvalidArgument("length"))?,
        }
        .encode_request(request_id)
        .map_err(NetworkClientError::Protocol)?;

        let response_payload = self.connection.send_request_and_wait(payload)?;
        let response = ReadAtResponse::decode_response(
            &response_payload,
            request_id,
            self.session_id,
            buffer.len() as u32,
        )
        .map_err(map_data_plane_error)?;
        buffer.copy_from_slice(&response.data);
        Ok(())
    }

    pub fn write_at(&self, offset: u64, data: &[u8]) -> Result<(), NetworkClientError> {
        self.ensure_usable()?;
        if self.read_only {
            return Err(NetworkClientError::ReadOnlySession);
        }
        validate_single_io(self.disk_size_bytes, self.max_io_bytes, offset, data.len())?;
        if data.is_empty() {
            return Ok(());
        }

        let request_id = self.connection.allocate_request_id();
        let payload = WriteAtRequest {
            session_id: self.session_id,
            offset,
            data: data.to_vec(),
        }
        .encode_request(request_id)
        .map_err(NetworkClientError::Protocol)?;

        let response_payload = self.connection.send_request_and_wait(payload)?;
        WriteAtRequest::decode_response(&response_payload, request_id, self.session_id)
            .map_err(map_data_plane_error)
    }
}

fn validate_single_io(
    disk_size_bytes: u64,
    max_io_bytes: u32,
    offset: u64,
    length: usize,
) -> Result<(), NetworkClientError> {
    let length_u64 =
        u64::try_from(length).map_err(|_| NetworkClientError::InvalidArgument("length"))?;
    if length == 0 {
        return Ok(());
    }
    if length > max_io_bytes as usize {
        return Err(NetworkClientError::InvalidArgument("length"));
    }

    let end = offset
        .checked_add(length_u64)
        .ok_or(NetworkClientError::InvalidArgument("offset"))?;
    if end > disk_size_bytes {
        return Err(NetworkClientError::InvalidArgument("offset"));
    }
    Ok(())
}

fn map_data_plane_error(error: ProtocolClientError) -> NetworkClientError {
    match error {
        ProtocolClientError::GatewayStatus(ProtocolStatusCode::ErrSessionExpired) => {
            NetworkClientError::SessionExpired
        }
        ProtocolClientError::GatewayStatus(ProtocolStatusCode::ErrSessionClosed)
        | ProtocolClientError::GatewayStatus(ProtocolStatusCode::ErrSessionNotFound) => {
            NetworkClientError::SessionClosed
        }
        ProtocolClientError::GatewayStatus(ProtocolStatusCode::ErrIoOutOfRange) => {
            NetworkClientError::IoOutOfRange
        }
        ProtocolClientError::GatewayStatus(ProtocolStatusCode::ErrIoTooLarge) => {
            NetworkClientError::IoTooLarge
        }
        ProtocolClientError::GatewayStatus(ProtocolStatusCode::ErrIoReadOnly) => {
            NetworkClientError::ReadOnlySession
        }
        ProtocolClientError::GatewayStatus(ProtocolStatusCode::ErrIoFailed) => {
            NetworkClientError::IoFailed
        }
        other => NetworkClientError::Protocol(other),
    }
}

#[cfg(test)]
mod tests {
    use super::DiskSession;
    use super::map_data_plane_error;
    use crate::network::ClientOperationCode;
    use crate::network::FLAG_RESPONSE;
    use crate::network::GatewayConnection;
    use crate::network::HEADER_SIZE;
    use crate::network::NetworkClientError;
    use crate::network::PROTOCOL_VERSION;
    use crate::network::ProtocolClientError;
    use crate::network::ProtocolHeader;
    use crate::network::ProtocolStatusCode;
    use crate::network::TransportEndpoint;
    use crate::network::transport_client::MAX_FRAME_PAYLOAD_BYTES;
    use crate::network::transport_client::read_frame_into;
    use crate::network::transport_client::write_frame;
    use std::net::TcpListener;
    use std::thread;

    #[test]
    fn disk_session_saves_session_metadata_and_lifecycle() {
        let connection = GatewayConnection::new(TransportEndpoint::new("127.0.0.1:1"));
        let session = DiskSession::new(connection, "A1b2C3d4E5f6G7h8", 77, 4096, true, 60_000, 300)
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
        let session =
            DiskSession::new(connection, "A1b2C3d4E5f6G7h8", 77, 4096, false, 60_000, 300)
                .expect("session should build");
        let cloned = session.clone();

        cloned.mark_closed();

        assert!(session.is_closed());
        assert_eq!(
            session
                .ensure_usable()
                .expect_err("closed session should fail")
                .to_string(),
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

    #[test]
    fn disk_session_uses_read_and_write_data_plane_requests() {
        let listener = TcpListener::bind("127.0.0.1:0").expect("bind should succeed");
        let address = listener.local_addr().expect("local addr should succeed");

        let server = thread::spawn(move || {
            let (mut stream, _) = listener.accept().expect("accept should succeed");
            let mut buffer = vec![0u8; MAX_FRAME_PAYLOAD_BYTES];

            let read_request = read_frame_into(&mut stream, &mut buffer)
                .expect("read request should succeed")
                .to_vec();
            let read_header =
                crate::network::parse_request_header(&read_request).expect("parse read request");
            assert_eq!(read_header.op_code, ClientOperationCode::ReadAt);
            assert_eq!(
                &read_request[HEADER_SIZE..HEADER_SIZE + 8],
                &8u64.to_be_bytes()
            );
            assert_eq!(
                &read_request[HEADER_SIZE + 8..HEADER_SIZE + 12],
                &4u32.to_be_bytes()
            );

            let read_response = ProtocolHeader {
                protocol_version: PROTOCOL_VERSION,
                header_len: HEADER_SIZE as u8,
                op_code: ClientOperationCode::ReadAt,
                flags: FLAG_RESPONSE,
                status_code: ProtocolStatusCode::Ok,
                reserved: 0,
                request_id: read_header.request_id,
                session_id: read_header.session_id,
            }
            .encode(b"YUME");
            write_frame(&mut stream, &read_response).expect("write read response");

            let write_request = read_frame_into(&mut stream, &mut buffer)
                .expect("write request should succeed")
                .to_vec();
            let write_header =
                crate::network::parse_request_header(&write_request).expect("parse write request");
            assert_eq!(write_header.op_code, ClientOperationCode::WriteAt);
            assert_eq!(
                &write_request[HEADER_SIZE..HEADER_SIZE + 8],
                &12u64.to_be_bytes()
            );
            assert_eq!(
                &write_request[HEADER_SIZE + 8..HEADER_SIZE + 12],
                &4u32.to_be_bytes()
            );
            assert_eq!(&write_request[HEADER_SIZE + 12..], b"DISK");

            let write_response = ProtocolHeader {
                protocol_version: PROTOCOL_VERSION,
                header_len: HEADER_SIZE as u8,
                op_code: ClientOperationCode::WriteAt,
                flags: FLAG_RESPONSE,
                status_code: ProtocolStatusCode::Ok,
                reserved: 0,
                request_id: write_header.request_id,
                session_id: write_header.session_id,
            }
            .encode(&[]);
            write_frame(&mut stream, &write_response).expect("write write response");
        });

        let connection = GatewayConnection::new(TransportEndpoint::new(address.to_string()));
        connection.connect().expect("connect should succeed");
        let session = DiskSession::new(
            connection.clone(),
            "A1b2C3d4E5f6G7h8",
            77,
            4096,
            false,
            60_000,
            300,
        )
        .expect("session should build");

        let mut read_buffer = [0u8; 4];
        session
            .read_at(8, &mut read_buffer)
            .expect("read should succeed");
        assert_eq!(&read_buffer, b"YUME");

        session.write_at(12, b"DISK").expect("write should succeed");

        connection.close().expect("close should succeed");
        server.join().expect("server should join");
    }

    #[test]
    fn disk_session_close_uses_close_request_and_marks_closed() {
        let listener = TcpListener::bind("127.0.0.1:0").expect("bind should succeed");
        let address = listener.local_addr().expect("local addr should succeed");

        let server = thread::spawn(move || {
            let (mut stream, _) = listener.accept().expect("accept should succeed");
            let mut buffer = vec![0u8; MAX_FRAME_PAYLOAD_BYTES];

            let close_request = read_frame_into(&mut stream, &mut buffer)
                .expect("close request should succeed")
                .to_vec();
            let close_header =
                crate::network::parse_request_header(&close_request).expect("parse close request");
            assert_eq!(close_header.op_code, ClientOperationCode::Close);
            assert!(close_request[HEADER_SIZE..].is_empty());

            let close_response = ProtocolHeader {
                protocol_version: PROTOCOL_VERSION,
                header_len: HEADER_SIZE as u8,
                op_code: ClientOperationCode::Close,
                flags: FLAG_RESPONSE,
                status_code: ProtocolStatusCode::Ok,
                reserved: 0,
                request_id: close_header.request_id,
                session_id: close_header.session_id,
            }
            .encode(&[]);
            write_frame(&mut stream, &close_response).expect("write close response");
        });

        let connection = GatewayConnection::new(TransportEndpoint::new(address.to_string()));
        connection.connect().expect("connect should succeed");
        let session = DiskSession::new(
            connection.clone(),
            "A1b2C3d4E5f6G7h8",
            77,
            4096,
            false,
            60_000,
            300,
        )
        .expect("session should build");

        session.close().expect("close should succeed");
        assert!(session.is_closed());

        connection.close().expect("close should succeed");
        server.join().expect("server should join");
    }

    #[test]
    fn disk_session_maps_gateway_io_statuses_to_network_errors() {
        assert_eq!(
            map_data_plane_error(ProtocolClientError::GatewayStatus(
                ProtocolStatusCode::ErrIoOutOfRange
            )),
            NetworkClientError::IoOutOfRange
        );
        assert_eq!(
            map_data_plane_error(ProtocolClientError::GatewayStatus(
                ProtocolStatusCode::ErrIoTooLarge
            )),
            NetworkClientError::IoTooLarge
        );
        assert_eq!(
            map_data_plane_error(ProtocolClientError::GatewayStatus(
                ProtocolStatusCode::ErrIoReadOnly
            )),
            NetworkClientError::ReadOnlySession
        );
        assert_eq!(
            map_data_plane_error(ProtocolClientError::GatewayStatus(
                ProtocolStatusCode::ErrIoFailed
            )),
            NetworkClientError::IoFailed
        );
    }
}
