use std::sync::Arc;
use std::sync::Mutex;
use std::sync::atomic::AtomicU8;
use std::sync::atomic::Ordering;

use super::error::NetworkClientError;
use super::gateway_connection::GatewayConnection;
use super::protocol_client::CloseRequest;
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
    session_id: u64,
    lifecycle: Arc<DiskSessionLifecycle>,
}

#[derive(Debug)]
struct DiskSessionLifecycle {
    terminal_error: Mutex<Option<NetworkClientError>>,
    state: AtomicU8,
}

impl DiskSession {
    pub fn new(
        connection: Arc<GatewayConnection>,
        session_id: u64,
    ) -> Result<Self, NetworkClientError> {
        if session_id == 0 {
            return Err(NetworkClientError::InvalidArgument("session_id"));
        }
        if !connection.is_session_active(session_id) {
            return Err(NetworkClientError::InvalidState("session_id"));
        }

        Ok(Self {
            connection,
            session_id,
            lifecycle: Arc::new(DiskSessionLifecycle {
                terminal_error: Mutex::new(None),
                state: AtomicU8::new(STATE_OPEN),
            }),
        })
    }

    pub fn connection(&self) -> &Arc<GatewayConnection> {
        &self.connection
    }

    pub fn session_id(&self) -> u64 {
        self.session_id
    }

    pub fn is_closed(&self) -> bool {
        self.lifecycle.state.load(Ordering::Acquire) == STATE_CLOSED
    }

    pub fn is_connection_alive(&self) -> bool {
        self.connection.is_connected()
    }

    pub fn is_terminal(&self) -> bool {
        self.lifecycle
            .terminal_error
            .lock()
            .expect("disk session terminal_error poisoned")
            .is_some()
    }

    pub(crate) fn mark_closed(&self) {
        self.lifecycle.state.store(STATE_CLOSED, Ordering::Release);
        self.connection.clear_session(self.session_id);
    }

    pub fn ensure_usable(&self) -> Result<(), NetworkClientError> {
        if self.is_closed() {
            return Err(NetworkClientError::SessionUnavailable);
        }

        if let Some(error) = self
            .lifecycle
            .terminal_error
            .lock()
            .expect("disk session terminal_error poisoned")
            .clone()
        {
            return Err(error);
        }

        if !self.is_connection_alive() || !self.connection.is_session_active(self.session_id) {
            self.mark_terminal(NetworkClientError::SessionUnavailable);
            return Err(NetworkClientError::SessionUnavailable);
        }
        Ok(())
    }

    pub fn close(&self) -> Result<(), NetworkClientError> {
        if self.is_closed() {
            return Ok(());
        }

        if let Err(error) = self.ensure_usable() {
            if matches!(error, NetworkClientError::SessionUnavailable) {
                self.mark_closed();
                return Ok(());
            }
            return Err(error);
        }

        let request_id = self.connection.allocate_request_id();
        let payload = CloseRequest {
            session_id: self.session_id,
        }
        .encode_request(request_id)
        .map_err(NetworkClientError::Protocol)?;

        let response_payload = self.connection.send_request_and_wait(payload)?;
        let close_result =
            CloseRequest::decode_response(&response_payload, request_id, self.session_id)
                .map_err(map_data_plane_error);
        self.mark_closed();
        close_result
    }

    pub fn read_at(&self, offset: u64, buffer: &mut [u8]) -> Result<(), NetworkClientError> {
        self.ensure_usable()?;
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
        .map_err(map_request_error(self))?;
        buffer.copy_from_slice(&response.data);
        Ok(())
    }

    pub fn write_at(&self, offset: u64, data: &[u8]) -> Result<(), NetworkClientError> {
        self.ensure_usable()?;
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
            .map_err(map_request_error(self))?;
        Ok(())
    }

    fn mark_terminal(&self, error: NetworkClientError) {
        self.lifecycle.state.store(STATE_CLOSED, Ordering::Release);
        let mut terminal_error = self
            .lifecycle
            .terminal_error
            .lock()
            .expect("disk session terminal_error poisoned");
        if terminal_error.is_none() {
            *terminal_error = Some(error);
        }
        self.connection.clear_session(self.session_id);
    }
}

fn map_request_error<'a>(
    session: &'a DiskSession,
) -> impl FnOnce(ProtocolClientError) -> NetworkClientError + 'a {
    move |error| {
        let mapped = map_data_plane_error(error);
        if is_terminal_session_error(&mapped) {
            session.mark_terminal(mapped.clone());
        }
        mapped
    }
}

fn is_terminal_session_error(error: &NetworkClientError) -> bool {
    matches!(error, NetworkClientError::SessionUnavailable)
}

fn map_data_plane_error(error: ProtocolClientError) -> NetworkClientError {
    match error {
        ProtocolClientError::GatewayStatus(ProtocolStatusCode::ErrSessionUnavailable) => {
            NetworkClientError::SessionUnavailable
        }
        ProtocolClientError::GatewayStatus(ProtocolStatusCode::ErrIoOutOfRange) => {
            NetworkClientError::InvalidIo("out_of_range")
        }
        ProtocolClientError::GatewayStatus(ProtocolStatusCode::ErrIoTooLarge) => {
            NetworkClientError::InvalidIo("too_large")
        }
        ProtocolClientError::GatewayStatus(ProtocolStatusCode::ErrIoReadOnly) => {
            NetworkClientError::InvalidIo("read_only")
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
    use crate::network::expect_client_hello;
    use crate::network::transport_client::MAX_FRAME_PAYLOAD_BYTES;
    use crate::network::transport_client::read_frame_into;
    use crate::network::transport_client::write_frame;
    use std::net::TcpListener;
    use std::sync::Arc;
    use std::thread;

    fn staged_connection(endpoint: TransportEndpoint, session_id: u64) -> Arc<GatewayConnection> {
        let connection = GatewayConnection::new(endpoint);
        connection
            .begin_session_open()
            .expect("begin session open should succeed");
        connection
            .finish_session_open(session_id)
            .expect("finish session open should succeed");
        connection
    }

    #[test]
    fn disk_session_tracks_session_handle_and_lifecycle() {
        let connection = staged_connection(TransportEndpoint::new("127.0.0.1:1"), 77);
        let session = DiskSession::new(connection, 77).expect("session should build");

        assert_eq!(session.session_id(), 77);
        assert!(!session.is_closed());
    }

    #[test]
    fn disk_session_shares_closed_state_across_clones() {
        let connection = staged_connection(TransportEndpoint::new("127.0.0.1:1"), 77);
        let session = DiskSession::new(connection, 77).expect("session should build");
        let cloned = session.clone();

        cloned.mark_closed();

        assert!(session.is_closed());
        assert_eq!(
            session
                .ensure_usable()
                .expect_err("closed session should fail")
                .to_string(),
            "session-unavailable"
        );
    }

    #[test]
    fn disk_session_rejects_invalid_inputs() {
        let connection = GatewayConnection::new(TransportEndpoint::new("127.0.0.1:1"));

        let error =
            DiskSession::new(connection.clone(), 0).expect_err("zero session id should fail");
        assert_eq!(error.to_string(), "invalid-argument: session_id");

        let error = DiskSession::new(connection, 77).expect_err("inactive session should fail");
        assert_eq!(error.to_string(), "invalid-state: session_id");
    }

    #[test]
    fn disk_session_uses_read_and_write_data_plane_requests() {
        let listener = TcpListener::bind("127.0.0.1:0").expect("bind should succeed");
        let address = listener.local_addr().expect("local addr should succeed");

        let server = thread::spawn(move || {
            let (mut stream, _) = listener.accept().expect("accept should succeed");
            expect_client_hello(&mut stream);
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

        let connection = staged_connection(TransportEndpoint::new(address.to_string()), 77);
        connection.connect().expect("connect should succeed");
        let session = DiskSession::new(connection.clone(), 77).expect("session should build");

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
            expect_client_hello(&mut stream);
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

        let connection = staged_connection(TransportEndpoint::new(address.to_string()), 77);
        connection.connect().expect("connect should succeed");
        let session = DiskSession::new(connection.clone(), 77).expect("session should build");

        session.close().expect("close should succeed");
        assert!(session.is_closed());

        connection.close().expect("close should succeed");
        server.join().expect("server should join");
    }

    #[test]
    fn disk_session_marks_itself_terminal_after_session_is_cleared() {
        let connection = staged_connection(TransportEndpoint::new("127.0.0.1:1"), 77);
        let session = DiskSession::new(connection.clone(), 77).expect("session should build");

        connection.clear_session(77);

        let error = session
            .ensure_usable()
            .expect_err("cleared session should fail");
        assert_eq!(error.to_string(), "session-unavailable");
        assert!(session.is_terminal());
        assert!(session.is_closed());
    }

    #[test]
    fn disk_session_maps_gateway_io_statuses_to_network_errors() {
        assert_eq!(
            map_data_plane_error(ProtocolClientError::GatewayStatus(
                ProtocolStatusCode::ErrIoOutOfRange
            )),
            NetworkClientError::InvalidIo("out_of_range")
        );
        assert_eq!(
            map_data_plane_error(ProtocolClientError::GatewayStatus(
                ProtocolStatusCode::ErrIoTooLarge
            )),
            NetworkClientError::InvalidIo("too_large")
        );
        assert_eq!(
            map_data_plane_error(ProtocolClientError::GatewayStatus(
                ProtocolStatusCode::ErrIoReadOnly
            )),
            NetworkClientError::InvalidIo("read_only")
        );
        assert_eq!(
            map_data_plane_error(ProtocolClientError::GatewayStatus(
                ProtocolStatusCode::ErrIoFailed
            )),
            NetworkClientError::IoFailed
        );
    }
}
