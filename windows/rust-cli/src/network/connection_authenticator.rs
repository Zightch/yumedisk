use std::sync::Arc;

use super::crypto_win32;
use super::error::NetworkClientError;
use super::gateway_connection::GatewayConnection;
use super::protocol_client::AuthFinishRequest;
use super::protocol_client::AuthStartRequest;
use super::protocol_client::AuthStartResponse;
use super::protocol_client::ProtocolClientError;
use super::protocol_client::ProtocolStatusCode;

#[derive(Debug, Clone)]
pub struct ConnectionAuthenticator {
    connection: Arc<GatewayConnection>,
}

impl ConnectionAuthenticator {
    pub fn new(connection: Arc<GatewayConnection>) -> Self {
        Self { connection }
    }

    pub fn connection(&self) -> &Arc<GatewayConnection> {
        &self.connection
    }

    pub fn authenticate(&self, claim_code: &str) -> Result<String, NetworkClientError> {
        let material = parse_claim_code(claim_code)?;
        self.connection.begin_auth(&material.disk_id)?;

        let start_request_id = self.connection.allocate_request_id();
        let start_payload = AuthStartRequest {
            disk_id: material.disk_id.clone(),
        }
        .encode_request(start_request_id)
        .map_err(NetworkClientError::Protocol)?;
        let start_response_payload = match self.connection.send_request_and_wait(start_payload) {
            Ok(payload) => payload,
            Err(error) => {
                self.connection.fail_auth();
                return Err(error);
            }
        };
        let start_response =
            match AuthStartResponse::decode_response(&start_response_payload, start_request_id) {
                Ok(response) => response,
                Err(error) => {
                    self.connection.fail_auth();
                    return Err(NetworkClientError::Protocol(error));
                }
            };

        let proof = match compute_proof(material.auth_verifier, &start_response) {
            Ok(proof) => proof,
            Err(error) => {
                self.connection.fail_auth();
                return Err(error);
            }
        };
        let finish_request_id = self.connection.allocate_request_id();
        let finish_payload = AuthFinishRequest {
            challenge_token: start_response.challenge_token,
            proof,
        }
        .encode_request(finish_request_id)
        .map_err(|error| {
            self.connection.fail_auth();
            NetworkClientError::Protocol(error)
        })?;
        let finish_response_payload = match self.connection.send_request_and_wait(finish_payload) {
            Ok(payload) => payload,
            Err(error) => {
                self.connection.fail_auth();
                return Err(error);
            }
        };
        if let Err(error) =
            AuthFinishRequest::decode_response(&finish_response_payload, finish_request_id)
                .map_err(map_auth_finish_error(&material.disk_id))
        {
            self.connection.fail_auth();
            return Err(error);
        }

        if let Err(error) = self.connection.finish_auth(&material.disk_id) {
            self.connection.fail_auth();
            return Err(error);
        }
        Ok(material.disk_id)
    }
}

#[derive(Debug)]
struct ClaimCodeMaterial {
    disk_id: String,
    auth_verifier: [u8; 64],
}

fn parse_claim_code(claim_code: &str) -> Result<ClaimCodeMaterial, NetworkClientError> {
    const DISK_ID_BYTES: usize = 16;
    const MIN_CLAIM_SECRET_BYTES: usize = 64;
    const MIN_CLAIM_CODE_BYTES: usize = DISK_ID_BYTES + MIN_CLAIM_SECRET_BYTES;

    let bytes = claim_code.as_bytes();
    if bytes.len() < MIN_CLAIM_CODE_BYTES {
        return Err(NetworkClientError::InvalidArgument("claim_code"));
    }
    if !bytes.iter().all(|byte| byte.is_ascii_alphanumeric()) {
        return Err(NetworkClientError::InvalidArgument("claim_code"));
    }

    let auth_verifier = crypto_win32::sha512(bytes)?;

    Ok(ClaimCodeMaterial {
        disk_id: claim_code[..DISK_ID_BYTES].to_string(),
        auth_verifier,
    })
}

fn compute_proof(
    auth_verifier: [u8; 64],
    response: &AuthStartResponse,
) -> Result<[u8; 64], NetworkClientError> {
    crypto_win32::hmac_sha512(&auth_verifier, &response.salt)
}

fn map_auth_finish_error<'a>(
    disk_id: &'a str,
) -> impl FnOnce(ProtocolClientError) -> NetworkClientError + 'a {
    move |error| match error {
        ProtocolClientError::GatewayStatus(ProtocolStatusCode::ErrAuthFailed) => {
            NetworkClientError::UnauthorizedDisk {
                disk_id: disk_id.to_string(),
            }
        }
        ProtocolClientError::GatewayStatus(ProtocolStatusCode::ErrInvalidRequest) => {
            NetworkClientError::InvalidState("auth_finish")
        }
        other => NetworkClientError::Protocol(other),
    }
}

#[cfg(test)]
mod tests {
    use super::ConnectionAuthenticator;
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
    fn authenticate_completes_start_and_finish_and_marks_connection() {
        let claim_code =
            "A1b2C3d4E5f6G7h8abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789ab";
        let listener = TcpListener::bind("127.0.0.1:0").expect("bind should succeed");
        let address = listener.local_addr().expect("local addr should succeed");

        let server = thread::spawn(move || {
            let (mut stream, _) = listener.accept().expect("accept should succeed");
            let mut buffer = vec![0u8; MAX_FRAME_PAYLOAD_BYTES];

            let auth_start = read_frame_into(&mut stream, &mut buffer)
                .expect("read auth start should succeed")
                .to_vec();
            let start_header =
                crate::network::parse_request_header(&auth_start).expect("parse auth start");
            assert_eq!(start_header.op_code, ClientOperationCode::AuthStart);
            assert_eq!(&auth_start[HEADER_SIZE..], b"A1b2C3d4E5f6G7h8");

            let mut start_body = Vec::new();
            start_body.push(1);
            start_body.extend_from_slice(&30u16.to_be_bytes());
            start_body.extend_from_slice(&[5u8; 16]);
            start_body.extend_from_slice(&3u16.to_be_bytes());
            start_body.extend_from_slice(b"tok");
            let start_response = ProtocolHeader {
                protocol_version: PROTOCOL_VERSION,
                header_len: HEADER_SIZE as u8,
                op_code: ClientOperationCode::AuthStart,
                flags: FLAG_RESPONSE,
                status_code: ProtocolStatusCode::Ok,
                reserved: 0,
                request_id: start_header.request_id,
                session_id: 0,
            }
            .encode(&start_body);
            write_frame(&mut stream, &start_response).expect("write auth start response");

            let auth_finish = read_frame_into(&mut stream, &mut buffer)
                .expect("read auth finish should succeed")
                .to_vec();
            let finish_header =
                crate::network::parse_request_header(&auth_finish).expect("parse auth finish");
            assert_eq!(finish_header.op_code, ClientOperationCode::AuthFinish);
            assert_eq!(
                &auth_finish[HEADER_SIZE..HEADER_SIZE + 2],
                &3u16.to_be_bytes()
            );
            assert_eq!(&auth_finish[HEADER_SIZE + 2..HEADER_SIZE + 5], b"tok");

            let finish_response = ProtocolHeader {
                protocol_version: PROTOCOL_VERSION,
                header_len: HEADER_SIZE as u8,
                op_code: ClientOperationCode::AuthFinish,
                flags: FLAG_RESPONSE,
                status_code: ProtocolStatusCode::Ok,
                reserved: 0,
                request_id: finish_header.request_id,
                session_id: 0,
            }
            .encode(&1u64.to_be_bytes());
            write_frame(&mut stream, &finish_response).expect("write auth finish response");
        });

        let connection = GatewayConnection::new(TransportEndpoint::new(address.to_string()));
        connection.connect().expect("connect should succeed");
        let authenticator = ConnectionAuthenticator::new(connection.clone());

        let disk_id = authenticator
            .authenticate(claim_code)
            .expect("authenticate should succeed");
        assert_eq!(disk_id, "A1b2C3d4E5f6G7h8");
        assert!(connection.is_authorized("A1b2C3d4E5f6G7h8"));

        connection.close().expect("close should succeed");
        server.join().expect("server should join");
    }

    #[test]
    fn authenticate_maps_auth_failed_to_unauthorized_disk() {
        let claim_code =
            "A1b2C3d4E5f6G7h8abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789ab";
        let listener = TcpListener::bind("127.0.0.1:0").expect("bind should succeed");
        let address = listener.local_addr().expect("local addr should succeed");

        let server = thread::spawn(move || {
            let (mut stream, _) = listener.accept().expect("accept should succeed");
            let mut buffer = vec![0u8; MAX_FRAME_PAYLOAD_BYTES];

            let auth_start = read_frame_into(&mut stream, &mut buffer)
                .expect("read auth start should succeed")
                .to_vec();
            let start_header =
                crate::network::parse_request_header(&auth_start).expect("parse auth start");

            let mut start_body = Vec::new();
            start_body.push(1);
            start_body.extend_from_slice(&30u16.to_be_bytes());
            start_body.extend_from_slice(&[5u8; 16]);
            start_body.extend_from_slice(&3u16.to_be_bytes());
            start_body.extend_from_slice(b"tok");
            let start_response = ProtocolHeader {
                protocol_version: PROTOCOL_VERSION,
                header_len: HEADER_SIZE as u8,
                op_code: ClientOperationCode::AuthStart,
                flags: FLAG_RESPONSE,
                status_code: ProtocolStatusCode::Ok,
                reserved: 0,
                request_id: start_header.request_id,
                session_id: 0,
            }
            .encode(&start_body);
            write_frame(&mut stream, &start_response).expect("write auth start response");

            let auth_finish = read_frame_into(&mut stream, &mut buffer)
                .expect("read auth finish should succeed")
                .to_vec();
            let finish_header =
                crate::network::parse_request_header(&auth_finish).expect("parse auth finish");

            let finish_response = ProtocolHeader {
                protocol_version: PROTOCOL_VERSION,
                header_len: HEADER_SIZE as u8,
                op_code: ClientOperationCode::AuthFinish,
                flags: FLAG_RESPONSE,
                status_code: ProtocolStatusCode::ErrAuthFailed,
                reserved: 0,
                request_id: finish_header.request_id,
                session_id: 0,
            }
            .encode(&[]);
            write_frame(&mut stream, &finish_response).expect("write auth finish response");
        });

        let connection = GatewayConnection::new(TransportEndpoint::new(address.to_string()));
        connection.connect().expect("connect should succeed");
        let authenticator = ConnectionAuthenticator::new(connection.clone());

        let error = authenticator
            .authenticate(claim_code)
            .expect_err("authenticate should fail");
        assert_eq!(error.to_string(), "unauthorized-disk: A1b2C3d4E5f6G7h8");
        assert!(!connection.is_authorized("A1b2C3d4E5f6G7h8"));

        let _ = connection.close();
        server.join().expect("server should join");
    }

    #[test]
    fn authenticate_rejects_second_attempt_in_non_idle_phase() {
        let connection = GatewayConnection::new(TransportEndpoint::new("127.0.0.1:1"));
        connection
            .finish_auth("A1b2C3d4E5f6G7h8")
            .expect_err("finish auth without pending should fail");
        connection
            .begin_auth("A1b2C3d4E5f6G7h8")
            .expect("begin auth should succeed");

        let authenticator = ConnectionAuthenticator::new(connection);
        let claim_code =
            "A1b2C3d4E5f6G7h8abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789ab";

        let error = authenticator
            .authenticate(claim_code)
            .expect_err("second auth should fail");
        assert_eq!(error.to_string(), "invalid-state: auth_start");
    }
}
