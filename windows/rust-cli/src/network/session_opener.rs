use std::sync::Arc;

use super::disk_session::DiskSession;
use super::error::NetworkClientError;
use super::gateway_connection::GatewayConnection;
use super::protocol_client::ProtocolClientError;
use super::protocol_client::ProtocolStatusCode;
use super::protocol_client::SessionOpenRequest;
use super::protocol_client::SessionOpenResponse;

#[derive(Debug, Clone)]
pub struct SessionOpener {
    connection: Arc<GatewayConnection>,
}

impl SessionOpener {
    pub fn new(connection: Arc<GatewayConnection>) -> Self {
        Self { connection }
    }

    pub fn open(&self, disk_id: impl Into<String>) -> Result<DiskSession, NetworkClientError> {
        let disk_id = disk_id.into();
        if disk_id.is_empty() {
            return Err(NetworkClientError::InvalidArgument("disk_id"));
        }
        if !self.connection.is_authorized(&disk_id) {
            return Err(NetworkClientError::UnauthorizedDisk { disk_id });
        }

        let request_id = self.connection.allocate_request_id();
        let payload = SessionOpenRequest {
            disk_id: disk_id.clone(),
        }
        .encode_request(request_id)
        .map_err(NetworkClientError::Protocol)?;
        let response_payload = self.connection.send_request_and_wait(payload)?;
        let response = SessionOpenResponse::decode_response(&response_payload, request_id)
            .map_err(map_session_open_error(&disk_id))?;

        DiskSession::new(
            self.connection.clone(),
            disk_id,
            response.session_id,
            response.disk_size_bytes,
            response.read_only,
            response.max_io_bytes,
            response.session_ttl_seconds,
        )
    }
}

fn map_session_open_error<'a>(
    disk_id: &'a str,
) -> impl FnOnce(ProtocolClientError) -> NetworkClientError + 'a {
    move |error| match error {
        ProtocolClientError::GatewayStatus(ProtocolStatusCode::ErrAuthRequired) => {
            NetworkClientError::UnauthorizedDisk {
                disk_id: disk_id.to_string(),
            }
        }
        other => NetworkClientError::Protocol(other),
    }
}

#[cfg(test)]
mod tests {
    use super::SessionOpener;
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
    fn session_open_builds_disk_session_after_authorization() {
        let disk_id = "A1b2C3d4E5f6G7h8";
        let listener = TcpListener::bind("127.0.0.1:0").expect("bind should succeed");
        let address = listener.local_addr().expect("local addr should succeed");

        let server = thread::spawn(move || {
            let (mut stream, _) = listener.accept().expect("accept should succeed");
            let mut buffer = vec![0u8; MAX_FRAME_PAYLOAD_BYTES];

            let request = read_frame_into(&mut stream, &mut buffer)
                .expect("read session open should succeed")
                .to_vec();
            let header =
                crate::network::parse_request_header(&request).expect("parse session open");
            assert_eq!(header.op_code, ClientOperationCode::SessionOpen);
            assert_eq!(&request[HEADER_SIZE..], disk_id.as_bytes());

            let mut body = Vec::new();
            body.extend_from_slice(&4096u64.to_be_bytes());
            body.extend_from_slice(&60_000u32.to_be_bytes());
            body.extend_from_slice(&300u32.to_be_bytes());
            body.extend_from_slice(&1u16.to_be_bytes());
            body.extend_from_slice(&0u16.to_be_bytes());
            let response = ProtocolHeader {
                protocol_version: PROTOCOL_VERSION,
                header_len: HEADER_SIZE as u8,
                op_code: ClientOperationCode::SessionOpen,
                flags: FLAG_RESPONSE,
                status_code: ProtocolStatusCode::Ok,
                reserved: 0,
                request_id: header.request_id,
                session_id: 77,
            }
            .encode(&body);
            write_frame(&mut stream, &response).expect("write session open response");
        });

        let connection = GatewayConnection::new(TransportEndpoint::new(address.to_string()));
        connection.connect().expect("connect should succeed");
        connection.mark_authorized(disk_id);
        let opener = SessionOpener::new(connection.clone());

        let session = opener.open(disk_id).expect("open should succeed");
        assert_eq!(session.disk_id(), disk_id);
        assert_eq!(session.session_id(), 77);
        assert_eq!(session.disk_size_bytes(), 4096);
        assert_eq!(session.max_io_bytes(), 60_000);
        assert_eq!(session.session_ttl_seconds(), 300);
        assert!(session.read_only());

        connection.close().expect("close should succeed");
        server.join().expect("server should join");
    }

    #[test]
    fn session_open_rejects_unauthorized_disk_before_network_round_trip() {
        let connection = GatewayConnection::new(TransportEndpoint::new("127.0.0.1:1"));
        let opener = SessionOpener::new(connection);

        let error = opener
            .open("A1b2C3d4E5f6G7h8")
            .expect_err("open should fail");
        assert_eq!(error.to_string(), "unauthorized-disk: A1b2C3d4E5f6G7h8");
    }
}
