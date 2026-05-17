use std::collections::HashMap;
use std::fmt;
use std::sync::Arc;
use std::sync::Mutex;
use std::sync::atomic::AtomicBool;
use std::sync::atomic::AtomicU64;
use std::sync::atomic::Ordering;
use std::sync::mpsc;
use std::sync::mpsc::Receiver;
use std::thread;

use super::error::NetworkClientError;
use super::protocol_client::ClientOperationCode;
use super::protocol_client::SessionCloseNotice;
use super::protocol_client::parse_header;
use super::protocol_client::parse_request_header;
use super::transport_client::TransportClient;
use super::transport_client::TransportEndpoint;
use super::transport_client::TransportError;

pub struct GatewayConnection {
    endpoint: TransportEndpoint,
    transport: TransportClient,
    next_request_id: AtomicU64,
    pending_requests: Mutex<HashMap<u64, PendingRequest>>,
    session_notice_handler: Mutex<Option<Arc<dyn Fn(SessionCloseNotice) + Send + Sync>>>,
    phase: Mutex<ConnectionPhase>,
    receive_loop_started: AtomicBool,
}

impl fmt::Debug for GatewayConnection {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        formatter
            .debug_struct("GatewayConnection")
            .field("endpoint", &self.endpoint)
            .field("pending_request_count", &self.pending_request_count())
            .field("phase", &self.phase_name())
            .field("is_connected", &self.is_connected())
            .finish()
    }
}

#[derive(Debug)]
struct PendingRequest {
    op_code: ClientOperationCode,
    response_tx: mpsc::SyncSender<Result<Vec<u8>, NetworkClientError>>,
}

#[derive(Debug)]
pub struct GatewayResponseFuture {
    request_id: u64,
    response_rx: Receiver<Result<Vec<u8>, NetworkClientError>>,
}

#[derive(Debug, Clone, PartialEq, Eq)]
enum ConnectionPhase {
    Idle,
    AuthPending { disk_id: String },
    Authorized { disk_id: String },
    SessionOpen { disk_id: String, session_id: u64 },
}

impl GatewayResponseFuture {
    pub fn request_id(&self) -> u64 {
        self.request_id
    }

    pub fn recv(self) -> Result<Vec<u8>, NetworkClientError> {
        self.response_rx
            .recv()
            .unwrap_or(Err(NetworkClientError::SessionUnavailable))
    }
}

impl GatewayConnection {
    pub fn new(endpoint: TransportEndpoint) -> Arc<Self> {
        Arc::new(Self {
            transport: TransportClient::new(endpoint.clone()),
            endpoint,
            next_request_id: AtomicU64::new(1),
            pending_requests: Mutex::new(HashMap::new()),
            session_notice_handler: Mutex::new(None),
            phase: Mutex::new(ConnectionPhase::Idle),
            receive_loop_started: AtomicBool::new(false),
        })
    }

    pub fn endpoint(&self) -> &TransportEndpoint {
        &self.endpoint
    }

    pub fn transport(&self) -> &TransportClient {
        &self.transport
    }

    pub fn connect(self: &Arc<Self>) -> Result<(), NetworkClientError> {
        self.transport.connect().map_err(map_transport_error)?;
        self.start_receive_loop();
        Ok(())
    }

    pub fn is_connected(&self) -> bool {
        self.transport.is_connected()
    }

    pub fn close(&self) -> Result<(), NetworkClientError> {
        self.transport.close().map_err(map_transport_error)?;
        self.fail_all_pending(NetworkClientError::SessionUnavailable);
        self.reset_phase();
        Ok(())
    }

    pub fn set_session_notice_handler(
        &self,
        handler: Option<Arc<dyn Fn(SessionCloseNotice) + Send + Sync>>,
    ) {
        *self
            .session_notice_handler
            .lock()
            .expect("session_notice_handler poisoned") = handler;
    }

    pub fn allocate_request_id(&self) -> u64 {
        loop {
            let request_id = self.next_request_id.fetch_add(1, Ordering::Relaxed);
            if request_id != 0 {
                return request_id;
            }
        }
    }

    pub fn send_request(
        &self,
        payload: Vec<u8>,
    ) -> Result<GatewayResponseFuture, NetworkClientError> {
        let header = parse_request_header(&payload).map_err(NetworkClientError::Protocol)?;
        let (response_tx, response_rx) = mpsc::sync_channel(1);

        {
            let mut pending = self
                .pending_requests
                .lock()
                .expect("pending_requests poisoned");
            if pending.contains_key(&header.request_id) {
                return Err(NetworkClientError::PendingRequestConflict {
                    request_id: header.request_id,
                });
            }
            pending.insert(
                header.request_id,
                PendingRequest {
                    op_code: header.op_code,
                    response_tx,
                },
            );
        }

        if let Err(error) = self.transport.send_payload(payload) {
            let error = map_transport_error(error);
            let _ = self.take_pending_request(header.request_id);
            return Err(error);
        }

        Ok(GatewayResponseFuture {
            request_id: header.request_id,
            response_rx,
        })
    }

    pub fn send_request_and_wait(&self, payload: Vec<u8>) -> Result<Vec<u8>, NetworkClientError> {
        self.send_request(payload)?.recv()
    }

    pub fn pending_request_count(&self) -> usize {
        self.pending_requests
            .lock()
            .expect("pending_requests poisoned")
            .len()
    }

    pub fn begin_auth(&self, disk_id: &str) -> Result<(), NetworkClientError> {
        let mut phase = self.phase.lock().expect("phase poisoned");
        match &*phase {
            ConnectionPhase::Idle => {
                *phase = ConnectionPhase::AuthPending {
                    disk_id: disk_id.to_string(),
                };
                Ok(())
            }
            _ => Err(NetworkClientError::InvalidState("auth_start")),
        }
    }

    pub fn fail_auth(&self) {
        let mut phase = self.phase.lock().expect("phase poisoned");
        if matches!(*phase, ConnectionPhase::AuthPending { .. }) {
            *phase = ConnectionPhase::Idle;
        }
    }

    pub fn finish_auth(&self, disk_id: &str) -> Result<(), NetworkClientError> {
        let mut phase = self.phase.lock().expect("phase poisoned");
        match &*phase {
            ConnectionPhase::AuthPending {
                disk_id: pending_disk_id,
            } if pending_disk_id == disk_id => {
                *phase = ConnectionPhase::Authorized {
                    disk_id: disk_id.to_string(),
                };
                Ok(())
            }
            _ => Err(NetworkClientError::InvalidState("auth_finish")),
        }
    }

    pub fn begin_session_open(&self, disk_id: &str) -> Result<(), NetworkClientError> {
        let phase = self.phase.lock().expect("phase poisoned");
        match &*phase {
            ConnectionPhase::Authorized {
                disk_id: authorized_disk_id,
            } if authorized_disk_id == disk_id => Ok(()),
            ConnectionPhase::Authorized { .. } => Err(NetworkClientError::InvalidState("session_open")),
            ConnectionPhase::Idle | ConnectionPhase::AuthPending { .. } => {
                Err(NetworkClientError::InvalidState("session_open"))
            }
            ConnectionPhase::SessionOpen { .. } => Err(NetworkClientError::InvalidState("session_open")),
        }
    }

    pub fn finish_session_open(
        &self,
        disk_id: &str,
        session_id: u64,
    ) -> Result<(), NetworkClientError> {
        let mut phase = self.phase.lock().expect("phase poisoned");
        match &*phase {
            ConnectionPhase::Authorized {
                disk_id: authorized_disk_id,
            } if authorized_disk_id == disk_id && session_id != 0 => {
                *phase = ConnectionPhase::SessionOpen {
                    disk_id: disk_id.to_string(),
                    session_id,
                };
                Ok(())
            }
            _ => Err(NetworkClientError::InvalidState("session_open")),
        }
    }

    pub fn cancel_session_open(&self, disk_id: &str) {
        let mut phase = self.phase.lock().expect("phase poisoned");
        *phase = ConnectionPhase::Authorized {
            disk_id: disk_id.to_string(),
        };
    }

    pub fn require_open_session(
        &self,
        disk_id: Option<&str>,
        session_id: u64,
    ) -> Result<(), NetworkClientError> {
        let phase = self.phase.lock().expect("phase poisoned");
        match &*phase {
            ConnectionPhase::SessionOpen {
                disk_id: phase_disk_id,
                session_id: phase_session_id,
            } if *phase_session_id == session_id && disk_id.is_none_or(|value| value == phase_disk_id) => Ok(()),
            _ => Err(NetworkClientError::InvalidState("session_data_plane")),
        }
    }

    pub fn clear_session(&self, session_id: u64) {
        let mut phase = self.phase.lock().expect("phase poisoned");
        if let ConnectionPhase::SessionOpen { disk_id, session_id: current } = &*phase {
            if *current == session_id {
                *phase = ConnectionPhase::Authorized {
                    disk_id: disk_id.clone(),
                };
            }
        }
    }

    pub fn is_authorized(&self, disk_id: &str) -> bool {
        let phase = self.phase.lock().expect("phase poisoned");
        matches!(
            &*phase,
            ConnectionPhase::Authorized { disk_id: current }
                | ConnectionPhase::SessionOpen {
                    disk_id: current,
                    session_id: _,
                } if current == disk_id
        )
    }

    pub fn authorized_disk_id(&self) -> Option<String> {
        let phase = self.phase.lock().expect("phase poisoned");
        match &*phase {
            ConnectionPhase::Authorized { disk_id }
            | ConnectionPhase::SessionOpen {
                disk_id,
                session_id: _,
            } => Some(disk_id.clone()),
            _ => None,
        }
    }

    pub fn phase_name(&self) -> &'static str {
        let phase = self.phase.lock().expect("phase poisoned");
        match &*phase {
            ConnectionPhase::Idle => "idle",
            ConnectionPhase::AuthPending { .. } => "auth-pending",
            ConnectionPhase::Authorized { .. } => "authorized",
            ConnectionPhase::SessionOpen { .. } => "session-open",
        }
    }

    fn reset_phase(&self) {
        *self.phase.lock().expect("phase poisoned") = ConnectionPhase::Idle;
    }

    fn start_receive_loop(self: &Arc<Self>) {
        if self
            .receive_loop_started
            .compare_exchange(false, true, Ordering::AcqRel, Ordering::Acquire)
            .is_err()
        {
            return;
        }

        let connection = Arc::clone(self);
        thread::spawn(move || {
            loop {
                match connection.transport.recv_payload() {
                    Ok(payload) => {
                        if let Err(error) = connection.dispatch_response(payload) {
                            connection.fail_all_pending(error);
                            return;
                        }
                    }
                    Err(error) => {
                        connection.fail_all_pending(map_transport_error(error));
                        return;
                    }
                }
            }
        });
    }

    fn dispatch_response(&self, payload: Vec<u8>) -> Result<(), NetworkClientError> {
        let header = parse_header(&payload).map_err(NetworkClientError::Protocol)?;
        if header.op_code == ClientOperationCode::SessionCloseNotice {
            let notice =
                SessionCloseNotice::decode_notice(&payload).map_err(NetworkClientError::Protocol)?;
            self.clear_session(notice.session_id);
            if let Some(handler) = self
                .session_notice_handler
                .lock()
                .expect("session_notice_handler poisoned")
                .as_ref()
            {
                handler(notice);
            }
            return Ok(());
        }
        if !header.is_response() {
            return Err(NetworkClientError::Protocol(
                super::protocol_client::ProtocolClientError::UnexpectedFlags {
                    expected: super::protocol_client::FLAG_RESPONSE,
                    actual: header.flags,
                },
            ));
        }

        let pending = self.take_pending_request(header.request_id).ok_or(
            NetworkClientError::UnknownPendingRequest {
                request_id: header.request_id,
            },
        )?;

        if header.op_code != pending.op_code {
            let error = NetworkClientError::Protocol(
                super::protocol_client::ProtocolClientError::UnexpectedOpCode {
                    expected: Some(pending.op_code),
                    actual: header.op_code as u8,
                },
            );
            let _ = pending.response_tx.send(Err(error));
            return Ok(());
        }

        let _ = pending.response_tx.send(Ok(payload));
        Ok(())
    }

    fn take_pending_request(&self, request_id: u64) -> Option<PendingRequest> {
        self.pending_requests
            .lock()
            .expect("pending_requests poisoned")
            .remove(&request_id)
    }

    fn fail_all_pending(&self, error: NetworkClientError) {
        let pending = {
            let mut pending = self
                .pending_requests
                .lock()
                .expect("pending_requests poisoned");
            pending.drain().map(|(_, value)| value).collect::<Vec<_>>()
        };

        for request in pending {
            let _ = request.response_tx.send(Err(error.clone()));
        }
    }
}

fn map_transport_error(error: TransportError) -> NetworkClientError {
    match error {
        TransportError::AlreadyConnected => NetworkClientError::AlreadyConnected,
        TransportError::ConnectionClosed | TransportError::NotConnected => {
            NetworkClientError::SessionUnavailable
        }
        other => NetworkClientError::Transport(other.to_string()),
    }
}

#[cfg(test)]
mod tests {
    use super::GatewayConnection;
    use crate::network::ClientOperationCode;
    use crate::network::FLAG_RESPONSE;
    use crate::network::PROTOCOL_VERSION;
    use crate::network::ProtocolHeader;
    use crate::network::ProtocolStatusCode;
    use crate::network::TransportEndpoint;
    use crate::network::transport_client::MAX_FRAME_PAYLOAD_BYTES;
    use crate::network::transport_client::read_frame_into;
    use crate::network::transport_client::write_frame;
    use std::net::TcpListener;
    use std::sync::Arc;
    use std::thread;

    #[test]
    fn gateway_connection_pairs_responses_by_request_id_under_concurrency() {
        let listener = TcpListener::bind("127.0.0.1:0").expect("bind should succeed");
        let address = listener.local_addr().expect("local_addr should succeed");

        let server = thread::spawn(move || {
            let (mut stream, _) = listener.accept().expect("accept should succeed");
            let mut buffer = vec![0u8; MAX_FRAME_PAYLOAD_BYTES];

            let first = read_frame_into(&mut stream, &mut buffer)
                .expect("read first request should succeed")
                .to_vec();
            let second = read_frame_into(&mut stream, &mut buffer)
                .expect("read second request should succeed")
                .to_vec();

            let first_header =
                crate::network::parse_request_header(&first).expect("parse first header");
            let second_header =
                crate::network::parse_request_header(&second).expect("parse second header");

            let second_response = ProtocolHeader {
                protocol_version: PROTOCOL_VERSION,
                header_len: crate::network::HEADER_SIZE as u8,
                op_code: second_header.op_code,
                flags: FLAG_RESPONSE,
                status_code: ProtocolStatusCode::Ok,
                reserved: 0,
                request_id: second_header.request_id,
                session_id: second_header.session_id,
            }
            .encode(b"second");
            write_frame(&mut stream, &second_response)
                .expect("write second response should succeed");

            let first_response = ProtocolHeader {
                protocol_version: PROTOCOL_VERSION,
                header_len: crate::network::HEADER_SIZE as u8,
                op_code: first_header.op_code,
                flags: FLAG_RESPONSE,
                status_code: ProtocolStatusCode::Ok,
                reserved: 0,
                request_id: first_header.request_id,
                session_id: first_header.session_id,
            }
            .encode(b"first");
            write_frame(&mut stream, &first_response).expect("write first response should succeed");
        });

        let connection = GatewayConnection::new(TransportEndpoint::new(address.to_string()));
        connection.connect().expect("connect should succeed");

        let request_one = ProtocolHeader::new_request(ClientOperationCode::Ping, 1, 7)
            .expect("request one header")
            .encode(&1u64.to_be_bytes());
        let request_two = ProtocolHeader::new_request(ClientOperationCode::Close, 2, 7)
            .expect("request two header")
            .encode(&[]);

        let future_one = connection
            .send_request(request_one)
            .expect("request one send should succeed");
        let future_two = connection
            .send_request(request_two)
            .expect("request two send should succeed");

        let response_two = future_two.recv().expect("response two should succeed");
        let response_one = future_one.recv().expect("response one should succeed");

        assert_eq!(&response_two[crate::network::HEADER_SIZE..], b"second");
        assert_eq!(&response_one[crate::network::HEADER_SIZE..], b"first");
        assert_eq!(connection.pending_request_count(), 0);

        connection.close().expect("close should succeed");
        server.join().expect("server thread should join");
    }

    #[test]
    fn gateway_connection_fails_all_pending_when_transport_breaks() {
        let listener = TcpListener::bind("127.0.0.1:0").expect("bind should succeed");
        let address = listener.local_addr().expect("local_addr should succeed");

        let server = thread::spawn(move || {
            let (stream, _) = listener.accept().expect("accept should succeed");
            drop(stream);
        });

        let connection = GatewayConnection::new(TransportEndpoint::new(address.to_string()));
        connection.connect().expect("connect should succeed");

        let request = ProtocolHeader::new_request(ClientOperationCode::Ping, 1, 7)
            .expect("request header")
            .encode(&1u64.to_be_bytes());
        let future = connection
            .send_request(request)
            .expect("send should succeed");

        let error = future.recv().expect_err("pending request should fail");
        assert_eq!(error.to_string(), "session-unavailable");
        assert_eq!(connection.pending_request_count(), 0);

        server.join().expect("server thread should join");
    }

    #[test]
    fn gateway_connection_allocates_non_zero_request_ids() {
        let connection = GatewayConnection::new(TransportEndpoint::new("127.0.0.1:1"));
        assert_eq!(connection.allocate_request_id(), 1);
        assert_eq!(connection.allocate_request_id(), 2);
        assert_eq!(connection.allocate_request_id(), 3);
    }

    #[test]
    fn gateway_connection_rejects_duplicate_in_flight_request_id() {
        let listener = TcpListener::bind("127.0.0.1:0").expect("bind should succeed");
        let address = listener.local_addr().expect("local_addr should succeed");

        let server = thread::spawn(move || {
            let (_stream, _) = listener.accept().expect("accept should succeed");
            thread::sleep(std::time::Duration::from_millis(200));
        });

        let connection = Arc::new(GatewayConnection::new(TransportEndpoint::new(
            address.to_string(),
        )));
        connection.connect().expect("connect should succeed");

        let request = ProtocolHeader::new_request(ClientOperationCode::Ping, 9, 7)
            .expect("request header")
            .encode(&9u64.to_be_bytes());

        let _first = connection
            .send_request(request.clone())
            .expect("first send should succeed");
        let error = connection
            .send_request(request)
            .expect_err("duplicate request should fail");
        assert_eq!(error.to_string(), "pending-request-conflict: 9");

        let _ = connection.close();
        server.join().expect("server thread should join");
    }
}
