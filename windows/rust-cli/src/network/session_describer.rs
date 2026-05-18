use std::sync::Arc;

use super::error::NetworkClientError;
use super::gateway_connection::GatewayConnection;
use super::protocol_client::ProtocolClientError;
use super::protocol_client::ProtocolStatusCode;
use super::protocol_client::SessionDescribeRequest;
use super::protocol_client::SessionDescribeResponse;

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct SessionMetadata {
    pub disk_size_bytes: u64,
    pub max_io_bytes: u32,
    pub read_only: bool,
}

impl SessionMetadata {
    pub fn new(
        disk_size_bytes: u64,
        max_io_bytes: u32,
        read_only: bool,
    ) -> Result<Self, NetworkClientError> {
        if disk_size_bytes == 0 {
            return Err(NetworkClientError::InvalidArgument("disk_size_bytes"));
        }
        if max_io_bytes == 0 {
            return Err(NetworkClientError::InvalidArgument("max_io_bytes"));
        }

        Ok(Self {
            disk_size_bytes,
            max_io_bytes,
            read_only,
        })
    }
}

#[derive(Debug, Clone)]
pub struct SessionDescriber {
    connection: Arc<GatewayConnection>,
}

impl SessionDescriber {
    pub fn new(connection: Arc<GatewayConnection>) -> Self {
        Self { connection }
    }

    pub fn describe(&self, session_id: u64) -> Result<SessionMetadata, NetworkClientError> {
        if !self.connection.is_session_active(session_id) {
            return Err(NetworkClientError::SessionUnavailable);
        }

        let request_id = self.connection.allocate_request_id();
        let payload = SessionDescribeRequest { session_id }
            .encode_request(request_id)
            .map_err(NetworkClientError::Protocol)?;
        let response_payload = self.connection.send_request_and_wait(payload)?;
        let response =
            SessionDescribeResponse::decode_response(&response_payload, request_id, session_id)
                .map_err(map_session_describe_error)?;

        SessionMetadata::new(
            response.disk_size_bytes,
            response.max_io_bytes,
            response.read_only,
        )
    }
}

fn map_session_describe_error(error: ProtocolClientError) -> NetworkClientError {
    match error {
        ProtocolClientError::GatewayStatus(ProtocolStatusCode::ErrSessionUnavailable) => {
            NetworkClientError::SessionUnavailable
        }
        ProtocolClientError::GatewayStatus(ProtocolStatusCode::ErrInvalidRequest) => {
            NetworkClientError::InvalidState("session_describe")
        }
        other => NetworkClientError::Protocol(other),
    }
}

#[cfg(test)]
mod tests {
    use super::SessionDescriber;
    use super::SessionMetadata;
    use crate::network::ClientOperationCode;
    use crate::network::FLAG_RESPONSE;
    use crate::network::GatewayConnection;
    use crate::network::HEADER_SIZE;
    use crate::network::PROTOCOL_VERSION;
    use crate::network::ProtocolHeader;
    use crate::network::ProtocolStatusCode;
    use crate::network::TransportEndpoint;
    use crate::network::transport_client::MAX_FRAME_PAYLOAD_BYTES;
    use crate::network::transport_client::read_frame_into;
    use crate::network::transport_client::write_frame;
    use std::net::TcpListener;
    use std::thread;

    #[test]
    fn session_describe_reads_metadata_for_active_session() {
        let listener = TcpListener::bind("127.0.0.1:0").expect("bind should succeed");
        let address = listener.local_addr().expect("local addr should succeed");

        let server = thread::spawn(move || {
            let (mut stream, _) = listener.accept().expect("accept should succeed");
            let mut buffer = vec![0u8; MAX_FRAME_PAYLOAD_BYTES];

            let request = read_frame_into(&mut stream, &mut buffer)
                .expect("read describe request should succeed")
                .to_vec();
            let header =
                crate::network::parse_request_header(&request).expect("parse describe request");
            assert_eq!(header.op_code, ClientOperationCode::SessionDescribe);
            assert!(request[HEADER_SIZE..].is_empty());
            assert_eq!(header.session_id, 77);

            let mut body = Vec::new();
            body.extend_from_slice(&4096u64.to_be_bytes());
            body.extend_from_slice(&60_000u32.to_be_bytes());
            body.extend_from_slice(&1u16.to_be_bytes());
            body.extend_from_slice(&0u16.to_be_bytes());
            let response = ProtocolHeader {
                protocol_version: PROTOCOL_VERSION,
                header_len: HEADER_SIZE as u8,
                op_code: ClientOperationCode::SessionDescribe,
                flags: FLAG_RESPONSE,
                status_code: ProtocolStatusCode::Ok,
                reserved: 0,
                request_id: header.request_id,
                session_id: 77,
            }
            .encode(&body);
            write_frame(&mut stream, &response).expect("write describe response");
        });

        let connection = GatewayConnection::new(TransportEndpoint::new(address.to_string()));
        connection
            .begin_auth("A1b2C3d4E5f6G7h8")
            .expect("begin auth should succeed");
        connection
            .finish_auth("A1b2C3d4E5f6G7h8", 9)
            .expect("finish auth should succeed");
        connection
            .begin_session_open("A1b2C3d4E5f6G7h8", 9)
            .expect("begin open should succeed");
        connection
            .finish_session_open("A1b2C3d4E5f6G7h8", 9, 77)
            .expect("finish open should succeed");
        connection.connect().expect("connect should succeed");
        let describer = SessionDescriber::new(connection.clone());

        let metadata = describer.describe(77).expect("describe should succeed");
        assert_eq!(
            metadata,
            SessionMetadata {
                disk_size_bytes: 4096,
                max_io_bytes: 60_000,
                read_only: true,
            }
        );

        connection.close().expect("close should succeed");
        server.join().expect("server should join");
    }

    #[test]
    fn session_describe_rejects_inactive_session_before_network_round_trip() {
        let connection = GatewayConnection::new(TransportEndpoint::new("127.0.0.1:1"));
        let describer = SessionDescriber::new(connection);

        let error = describer.describe(77).expect_err("describe should fail");
        assert_eq!(error.to_string(), "session-unavailable");
    }
}
