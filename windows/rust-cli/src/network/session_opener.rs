use std::sync::Arc;

use super::connection_authenticator::AuthGrant;
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

    pub fn open(&self, grant: &AuthGrant) -> Result<u64, NetworkClientError> {
        self.connection
            .begin_session_open(grant.disk_id(), grant.auth_id())?;

        let request_id = self.connection.allocate_request_id();
        let payload = SessionOpenRequest {
            auth_id: grant.auth_id(),
        }
        .encode_request(request_id)
        .map_err(NetworkClientError::Protocol)?;
        let response_payload = match self.connection.send_request_and_wait(payload) {
            Ok(payload) => payload,
            Err(error) => {
                self.connection
                    .cancel_session_open(grant.disk_id(), grant.auth_id());
                return Err(error);
            }
        };
        let response = match SessionOpenResponse::decode_response(&response_payload, request_id)
            .map_err(map_session_open_error(grant.disk_id()))
        {
            Ok(response) => response,
            Err(error) => {
                self.connection
                    .cancel_session_open(grant.disk_id(), grant.auth_id());
                return Err(error);
            }
        };
        if let Err(error) = self.connection.finish_session_open(
            grant.disk_id(),
            grant.auth_id(),
            response.session_id,
        ) {
            self.connection
                .cancel_session_open(grant.disk_id(), grant.auth_id());
            return Err(error);
        }

        Ok(response.session_id)
    }
}

fn map_session_open_error<'a>(
    disk_id: &'a str,
) -> impl FnOnce(ProtocolClientError) -> NetworkClientError + 'a {
    move |error| match error {
        ProtocolClientError::GatewayStatus(ProtocolStatusCode::ErrAuthIdInvalid)
        | ProtocolClientError::GatewayStatus(ProtocolStatusCode::ErrAuthIdExpired) => {
            NetworkClientError::UnauthorizedDisk {
                disk_id: disk_id.to_string(),
            }
        }
        ProtocolClientError::GatewayStatus(ProtocolStatusCode::ErrInvalidRequest) => {
            NetworkClientError::InvalidState("session_open")
        }
        ProtocolClientError::GatewayStatus(ProtocolStatusCode::ErrSessionBusy) => {
            NetworkClientError::DiskBusy {
                disk_id: disk_id.to_string(),
            }
        }
        other => NetworkClientError::Protocol(other),
    }
}

#[cfg(test)]
mod tests {
    use super::SessionOpener;
    use crate::network::AuthGrant;
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
    use std::time::Duration;

    #[test]
    fn session_open_returns_session_id_after_authorization() {
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
            assert_eq!(&request[HEADER_SIZE..], &88u64.to_be_bytes());

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
            .encode(&[]);
            write_frame(&mut stream, &response).expect("write session open response");
            thread::sleep(Duration::from_millis(20));
        });

        let connection = GatewayConnection::new(TransportEndpoint::new(address.to_string()));
        connection.connect().expect("connect should succeed");
        connection
            .begin_auth(disk_id)
            .expect("begin auth should succeed");
        connection
            .finish_auth(disk_id, 88)
            .expect("finish auth should succeed");
        let opener = SessionOpener::new(connection.clone());
        let grant = AuthGrant::new(disk_id, 88).expect("grant should build");

        let session_id = opener.open(&grant).expect("open should succeed");
        assert_eq!(session_id, 77);
        assert!(connection.is_session_active(77));
        assert_eq!(connection.phase_name(), "idle");

        connection.close().expect("close should succeed");
        server.join().expect("server should join");
    }

    #[test]
    fn session_open_rejects_unauthorized_disk_before_network_round_trip() {
        let connection = GatewayConnection::new(TransportEndpoint::new("127.0.0.1:1"));
        let opener = SessionOpener::new(connection);
        let grant = AuthGrant::new("A1b2C3d4E5f6G7h8", 88).expect("grant should build");

        let error = opener.open(&grant).expect_err("open should fail");
        assert_eq!(error.to_string(), "invalid-state: session_open");
    }

    #[test]
    fn session_open_maps_busy_status_to_disk_busy() {
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

            let response = ProtocolHeader {
                protocol_version: PROTOCOL_VERSION,
                header_len: HEADER_SIZE as u8,
                op_code: ClientOperationCode::SessionOpen,
                flags: FLAG_RESPONSE,
                status_code: ProtocolStatusCode::ErrSessionBusy,
                reserved: 0,
                request_id: header.request_id,
                session_id: 0,
            }
            .encode(&[]);
            write_frame(&mut stream, &response).expect("write session open busy response");
            thread::sleep(Duration::from_millis(20));
        });

        let connection = GatewayConnection::new(TransportEndpoint::new(address.to_string()));
        connection.connect().expect("connect should succeed");
        connection
            .begin_auth(disk_id)
            .expect("begin auth should succeed");
        connection
            .finish_auth(disk_id, 88)
            .expect("finish auth should succeed");
        let opener = SessionOpener::new(connection.clone());
        let grant = AuthGrant::new(disk_id, 88).expect("grant should build");

        let error = opener.open(&grant).expect_err("open should fail");
        assert_eq!(error.to_string(), "disk-busy: A1b2C3d4E5f6G7h8");

        connection.close().expect("close should succeed");
        server.join().expect("server should join");
    }
}
