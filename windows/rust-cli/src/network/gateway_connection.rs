use std::collections::HashMap;
use std::collections::HashSet;
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
    lifecycle: Mutex<ConnectionLifecycle>,
    receive_loop_started: AtomicBool,
}

impl fmt::Debug for GatewayConnection {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        formatter
            .debug_struct("GatewayConnection")
            .field("endpoint", &self.endpoint)
            .field("pending_request_count", &self.pending_request_count())
            .field("phase", &self.phase_name())
            .field("active_session_count", &self.active_session_count())
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

#[derive(Debug)]
struct ConnectionLifecycle {
    auth_state: ConnectionPhase,
    active_sessions: HashSet<u64>,
}

#[derive(Debug, Clone, PartialEq, Eq)]
enum ConnectionPhase {
    Idle,
    AuthPending { disk_id: String },
    Authorized { disk_id: String, auth_id: u64 },
    SessionOpenPending { disk_id: String, auth_id: u64 },
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
            lifecycle: Mutex::new(ConnectionLifecycle {
                auth_state: ConnectionPhase::Idle,
                active_sessions: HashSet::new(),
            }),
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
        self.handle_disconnect(NetworkClientError::SessionUnavailable);
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
        let mut lifecycle = self.lifecycle.lock().expect("lifecycle poisoned");
        match &lifecycle.auth_state {
            ConnectionPhase::Idle => {
                lifecycle.auth_state = ConnectionPhase::AuthPending {
                    disk_id: disk_id.to_string(),
                };
                Ok(())
            }
            _ => Err(NetworkClientError::InvalidState("auth_start")),
        }
    }

    pub fn fail_auth(&self) {
        let mut lifecycle = self.lifecycle.lock().expect("lifecycle poisoned");
        if matches!(lifecycle.auth_state, ConnectionPhase::AuthPending { .. }) {
            lifecycle.auth_state = ConnectionPhase::Idle;
        }
    }

    pub fn finish_auth(&self, disk_id: &str, auth_id: u64) -> Result<(), NetworkClientError> {
        if auth_id == 0 {
            return Err(NetworkClientError::InvalidArgument("auth_id"));
        }

        let mut lifecycle = self.lifecycle.lock().expect("lifecycle poisoned");
        match &lifecycle.auth_state {
            ConnectionPhase::AuthPending {
                disk_id: pending_disk_id,
            } if pending_disk_id == disk_id => {
                lifecycle.auth_state = ConnectionPhase::Authorized {
                    disk_id: disk_id.to_string(),
                    auth_id,
                };
                Ok(())
            }
            _ => Err(NetworkClientError::InvalidState("auth_finish")),
        }
    }

    pub fn begin_session_open(
        &self,
        disk_id: &str,
        auth_id: u64,
    ) -> Result<(), NetworkClientError> {
        if auth_id == 0 {
            return Err(NetworkClientError::InvalidArgument("auth_id"));
        }

        let mut lifecycle = self.lifecycle.lock().expect("lifecycle poisoned");
        match &lifecycle.auth_state {
            ConnectionPhase::Authorized {
                disk_id: authorized_disk_id,
                auth_id: authorized_auth_id,
            } if authorized_disk_id == disk_id && *authorized_auth_id == auth_id => {
                lifecycle.auth_state = ConnectionPhase::SessionOpenPending {
                    disk_id: disk_id.to_string(),
                    auth_id,
                };
                Ok(())
            }
            _ => Err(NetworkClientError::InvalidState("session_open")),
        }
    }

    pub fn finish_session_open(
        &self,
        disk_id: &str,
        auth_id: u64,
        session_id: u64,
    ) -> Result<(), NetworkClientError> {
        if auth_id == 0 {
            return Err(NetworkClientError::InvalidArgument("auth_id"));
        }
        if session_id == 0 {
            return Err(NetworkClientError::InvalidArgument("session_id"));
        }

        let mut lifecycle = self.lifecycle.lock().expect("lifecycle poisoned");
        match &lifecycle.auth_state {
            ConnectionPhase::SessionOpenPending {
                disk_id: pending_disk_id,
                auth_id: pending_auth_id,
            } if pending_disk_id == disk_id && *pending_auth_id == auth_id => {
                lifecycle.auth_state = ConnectionPhase::Idle;
                lifecycle.active_sessions.insert(session_id);
                Ok(())
            }
            _ => Err(NetworkClientError::InvalidState("session_open")),
        }
    }

    pub fn cancel_session_open(&self, disk_id: &str, auth_id: u64) {
        let mut lifecycle = self.lifecycle.lock().expect("lifecycle poisoned");
        if matches!(
            &lifecycle.auth_state,
            ConnectionPhase::SessionOpenPending {
                disk_id: pending_disk_id,
                auth_id: pending_auth_id,
            } if pending_disk_id == disk_id && *pending_auth_id == auth_id
        ) {
            lifecycle.auth_state = ConnectionPhase::Authorized {
                disk_id: disk_id.to_string(),
                auth_id,
            };
        }
    }

    pub fn is_session_active(&self, session_id: u64) -> bool {
        self.lifecycle
            .lock()
            .expect("lifecycle poisoned")
            .active_sessions
            .contains(&session_id)
    }

    pub fn clear_session(&self, session_id: u64) {
        self.lifecycle
            .lock()
            .expect("lifecycle poisoned")
            .active_sessions
            .remove(&session_id);
    }

    pub fn is_authorized(&self, disk_id: &str) -> bool {
        let lifecycle = self.lifecycle.lock().expect("lifecycle poisoned");
        matches!(
            &lifecycle.auth_state,
            ConnectionPhase::Authorized {
                disk_id: current,
                auth_id: _,
            }
                | ConnectionPhase::SessionOpenPending {
                    disk_id: current,
                    auth_id: _,
                } if current == disk_id
        )
    }

    pub fn authorized_disk_id(&self) -> Option<String> {
        let lifecycle = self.lifecycle.lock().expect("lifecycle poisoned");
        match &lifecycle.auth_state {
            ConnectionPhase::Authorized {
                disk_id,
                auth_id: _,
            }
            | ConnectionPhase::SessionOpenPending {
                disk_id,
                auth_id: _,
            } => Some(disk_id.clone()),
            _ => None,
        }
    }

    pub fn phase_name(&self) -> &'static str {
        let lifecycle = self.lifecycle.lock().expect("lifecycle poisoned");
        match &lifecycle.auth_state {
            ConnectionPhase::Idle => "idle",
            ConnectionPhase::AuthPending { .. } => "auth-pending",
            ConnectionPhase::Authorized { .. } => "authorized",
            ConnectionPhase::SessionOpenPending { .. } => "session-open-pending",
        }
    }

    fn active_session_count(&self) -> usize {
        self.lifecycle
            .lock()
            .expect("lifecycle poisoned")
            .active_sessions
            .len()
    }

    fn reset_lifecycle(&self) {
        let mut lifecycle = self.lifecycle.lock().expect("lifecycle poisoned");
        lifecycle.auth_state = ConnectionPhase::Idle;
        lifecycle.active_sessions.clear();
    }

    fn handle_disconnect(&self, error: NetworkClientError) {
        self.fail_all_pending(error);
        self.reset_lifecycle();
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
                            connection.handle_disconnect(error);
                            return;
                        }
                    }
                    Err(error) => {
                        connection.handle_disconnect(map_transport_error(error));
                        return;
                    }
                }
            }
        });
    }

    fn dispatch_response(&self, payload: Vec<u8>) -> Result<(), NetworkClientError> {
        let header = parse_header(&payload).map_err(NetworkClientError::Protocol)?;
        if header.op_code == ClientOperationCode::SessionCloseNotice {
            let notice = SessionCloseNotice::decode_notice(&payload)
                .map_err(NetworkClientError::Protocol)?;
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
    use std::thread;

    #[test]
    fn gateway_connection_tracks_auth_grants_and_multiple_active_sessions() {
        let disk_one = "A1b2C3d4E5f6G7h8";
        let disk_two = "B1c2D3e4F5g6H7i8";
        let connection = GatewayConnection::new(TransportEndpoint::new("127.0.0.1:1"));

        connection
            .begin_auth(disk_one)
            .expect("first auth should begin");
        connection
            .finish_auth(disk_one, 11)
            .expect("first auth should finish");
        connection
            .begin_session_open(disk_one, 11)
            .expect("first open should begin");
        connection
            .finish_session_open(disk_one, 11, 101)
            .expect("first open should finish");

        connection
            .begin_auth(disk_two)
            .expect("second auth should begin even with an active session");
        connection
            .finish_auth(disk_two, 22)
            .expect("second auth should finish");
        connection
            .begin_session_open(disk_two, 22)
            .expect("second open should begin");
        connection
            .finish_session_open(disk_two, 22, 202)
            .expect("second open should finish");

        assert_eq!(connection.phase_name(), "idle");
        assert!(connection.is_session_active(101));
        assert!(connection.is_session_active(202));

        connection.clear_session(101);

        assert!(!connection.is_session_active(101));
        assert!(connection.is_session_active(202));
    }

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

        let request_one = ProtocolHeader::new_request(ClientOperationCode::ConnHeartbeat, 1, 0)
            .expect("request one header")
            .encode(&[]);
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

        let request = ProtocolHeader::new_request(ClientOperationCode::ConnHeartbeat, 1, 0)
            .expect("request header")
            .encode(&[]);
        let future = connection
            .send_request(request)
            .expect("send should succeed");

        let error = future.recv().expect_err("pending request should fail");
        assert_eq!(error.to_string(), "session-unavailable");
        assert_eq!(connection.pending_request_count(), 0);
        assert_eq!(connection.phase_name(), "idle");

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

        let connection = GatewayConnection::new(TransportEndpoint::new(address.to_string()));
        connection.connect().expect("connect should succeed");

        let request = ProtocolHeader::new_request(ClientOperationCode::ConnHeartbeat, 9, 0)
            .expect("request header")
            .encode(&[]);

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
