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
use std::time::Duration;

use super::ConnHeartbeatRequest;
use super::ConnHeartbeatResponse;
use super::error::NetworkClientError;
use super::hello_client::perform_client_hello;
use super::protocol_client::ClientOperationCode;
use super::protocol_client::SessionCloseNotice;
use super::protocol_client::parse_header;
use super::protocol_client::parse_request_header;
use super::transport_client::TransportClient;
use super::transport_client::TransportEndpoint;
use super::transport_client::TransportError;

#[cfg(not(test))]
const CONN_HEARTBEAT_INTERVAL: Duration = Duration::from_secs(5);
#[cfg(test)]
const CONN_HEARTBEAT_INTERVAL: Duration = Duration::from_millis(50);

#[cfg(not(test))]
const CONN_HEARTBEAT_TIMEOUT: Duration = Duration::from_secs(15);
#[cfg(test)]
const CONN_HEARTBEAT_TIMEOUT: Duration = Duration::from_millis(150);

pub struct GatewayConnection {
    endpoint: TransportEndpoint,
    transport: TransportClient,
    next_request_id: AtomicU64,
    pending_requests: Mutex<HashMap<u64, PendingRequest>>,
    disconnect_handler: Mutex<Option<Arc<dyn Fn() + Send + Sync>>>,
    session_notice_handler: Mutex<Option<Arc<dyn Fn(SessionCloseNotice) + Send + Sync>>>,
    lifecycle: Mutex<ConnectionLifecycle>,
    receive_loop_started: AtomicBool,
    heartbeat_loop_started: AtomicBool,
    disconnect_started: AtomicBool,
}

impl fmt::Debug for GatewayConnection {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        formatter
            .debug_struct("GatewayConnection")
            .field("endpoint", &self.endpoint)
            .field("pending_request_count", &self.pending_request_count())
            .field("phase", &self.phase_name())
            .field("auth_grant_count", &self.auth_grant_count())
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

#[derive(Debug, Default)]
struct ConnectionLifecycle {
    auth_in_flight: bool,
    open_in_flight: bool,
    active_sessions: HashSet<u64>,
    auth_grants: HashMap<u64, String>,
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

    pub fn recv_timeout(self, timeout: Duration) -> Result<Vec<u8>, NetworkClientError> {
        match self.response_rx.recv_timeout(timeout) {
            Ok(result) => result,
            Err(_) => Err(NetworkClientError::SessionUnavailable),
        }
    }
}

impl GatewayConnection {
    pub fn new(endpoint: TransportEndpoint) -> Arc<Self> {
        Arc::new(Self {
            transport: TransportClient::new(endpoint.clone()),
            endpoint,
            next_request_id: AtomicU64::new(1),
            pending_requests: Mutex::new(HashMap::new()),
            disconnect_handler: Mutex::new(None),
            session_notice_handler: Mutex::new(None),
            lifecycle: Mutex::new(ConnectionLifecycle::default()),
            receive_loop_started: AtomicBool::new(false),
            heartbeat_loop_started: AtomicBool::new(false),
            disconnect_started: AtomicBool::new(false),
        })
    }

    pub fn endpoint(&self) -> &TransportEndpoint {
        &self.endpoint
    }

    pub fn transport(&self) -> &TransportClient {
        &self.transport
    }

    pub fn connect(self: &Arc<Self>) -> Result<(), NetworkClientError> {
        let mut stream = self.transport.open_stream().map_err(map_transport_error)?;
        perform_client_hello(&mut stream)
            .map_err(|error| NetworkClientError::Transport(error.to_string()))?;
        self.transport
            .connect_with_stream(stream)
            .map_err(map_transport_error)?;
        self.start_receive_loop();
        self.start_heartbeat_loop();
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

    pub fn set_disconnect_handler(&self, handler: Option<Arc<dyn Fn() + Send + Sync>>) {
        *self
            .disconnect_handler
            .lock()
            .expect("disconnect_handler poisoned") = handler;
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

    pub fn begin_auth(&self) -> Result<(), NetworkClientError> {
        let mut lifecycle = self.lifecycle.lock().expect("lifecycle poisoned");
        if lifecycle.auth_in_flight || lifecycle.open_in_flight {
            return Err(NetworkClientError::InvalidState("auth_start"));
        }

        lifecycle.auth_in_flight = true;
        Ok(())
    }

    pub fn fail_auth(&self) {
        let mut lifecycle = self.lifecycle.lock().expect("lifecycle poisoned");
        lifecycle.auth_in_flight = false;
    }

    pub fn finish_auth(&self) -> Result<(), NetworkClientError> {
        let mut lifecycle = self.lifecycle.lock().expect("lifecycle poisoned");
        if !lifecycle.auth_in_flight || lifecycle.open_in_flight {
            return Err(NetworkClientError::InvalidState("auth_finish"));
        }

        lifecycle.auth_in_flight = false;
        Ok(())
    }

    pub fn begin_session_open(&self) -> Result<(), NetworkClientError> {
        let mut lifecycle = self.lifecycle.lock().expect("lifecycle poisoned");
        if lifecycle.auth_in_flight || lifecycle.open_in_flight {
            return Err(NetworkClientError::InvalidState("session_open"));
        }

        lifecycle.open_in_flight = true;
        Ok(())
    }

    pub fn finish_session_open(&self, session_id: u64) -> Result<(), NetworkClientError> {
        if session_id == 0 {
            return Err(NetworkClientError::InvalidArgument("session_id"));
        }

        let mut lifecycle = self.lifecycle.lock().expect("lifecycle poisoned");
        if !lifecycle.open_in_flight || lifecycle.auth_in_flight {
            return Err(NetworkClientError::InvalidState("session_open"));
        }

        lifecycle.open_in_flight = false;
        lifecycle.active_sessions.insert(session_id);
        Ok(())
    }

    pub fn register_auth_grant(
        &self,
        auth_id: u64,
        disk_id: String,
    ) -> Result<(), NetworkClientError> {
        if auth_id == 0 {
            return Err(NetworkClientError::InvalidArgument("auth_id"));
        }
        if disk_id.is_empty() {
            return Err(NetworkClientError::InvalidArgument("disk_id"));
        }

        self.lifecycle
            .lock()
            .expect("lifecycle poisoned")
            .auth_grants
            .insert(auth_id, disk_id);
        Ok(())
    }

    pub fn consume_auth_grant(&self, auth_id: u64) -> bool {
        self.lifecycle
            .lock()
            .expect("lifecycle poisoned")
            .auth_grants
            .remove(&auth_id)
            .is_some()
    }

    pub fn discard_auth_grant(&self, auth_id: u64) -> bool {
        self.consume_auth_grant(auth_id)
    }

    pub fn auth_grant_count(&self) -> usize {
        self.lifecycle
            .lock()
            .expect("lifecycle poisoned")
            .auth_grants
            .len()
    }

    pub fn should_close_after_session_close(&self) -> bool {
        let lifecycle = self.lifecycle.lock().expect("lifecycle poisoned");
        !lifecycle.auth_in_flight
            && !lifecycle.open_in_flight
            && lifecycle.active_sessions.is_empty()
            && lifecycle.auth_grants.is_empty()
    }

    pub fn fail_session_open(&self) {
        let mut lifecycle = self.lifecycle.lock().expect("lifecycle poisoned");
        lifecycle.open_in_flight = false;
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

    pub fn phase_name(&self) -> &'static str {
        let lifecycle = self.lifecycle.lock().expect("lifecycle poisoned");
        if lifecycle.auth_in_flight {
            "auth-pending"
        } else if lifecycle.open_in_flight {
            "session-open-pending"
        } else {
            "idle"
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
        lifecycle.auth_in_flight = false;
        lifecycle.open_in_flight = false;
        lifecycle.active_sessions.clear();
        lifecycle.auth_grants.clear();
    }

    fn handle_disconnect(&self, error: NetworkClientError) {
        if self
            .disconnect_started
            .compare_exchange(false, true, Ordering::AcqRel, Ordering::Acquire)
            .is_err()
        {
            return;
        }
        let _ = self.transport.close();
        if let Some(handler) = self
            .disconnect_handler
            .lock()
            .expect("disconnect_handler poisoned")
            .as_ref()
        {
            handler();
        }
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

    fn start_heartbeat_loop(self: &Arc<Self>) {
        if self
            .heartbeat_loop_started
            .compare_exchange(false, true, Ordering::AcqRel, Ordering::Acquire)
            .is_err()
        {
            return;
        }

        let connection = Arc::clone(self);
        thread::spawn(move || {
            loop {
                thread::sleep(CONN_HEARTBEAT_INTERVAL);
                if !connection.is_connected() {
                    return;
                }

                let request_id = connection.allocate_request_id();
                let payload = match ConnHeartbeatRequest.encode_request(request_id) {
                    Ok(payload) => payload,
                    Err(error) => {
                        connection.handle_disconnect(NetworkClientError::Protocol(error));
                        return;
                    }
                };
                let future = match connection.send_request(payload) {
                    Ok(future) => future,
                    Err(error) => {
                        connection.handle_disconnect(error);
                        return;
                    }
                };
                let response = match future.recv_timeout(CONN_HEARTBEAT_TIMEOUT) {
                    Ok(response) => response,
                    Err(error) => {
                        connection.handle_disconnect(error);
                        return;
                    }
                };
                if let Err(error) = ConnHeartbeatResponse::decode_response(&response, request_id) {
                    connection.handle_disconnect(NetworkClientError::Protocol(error));
                    return;
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
    use crate::network::FLAG_NOTICE;
    use crate::network::FLAG_RESPONSE;
    use crate::network::PROTOCOL_VERSION;
    use crate::network::ProtocolHeader;
    use crate::network::ProtocolStatusCode;
    use crate::network::TransportEndpoint;
    use crate::network::expect_client_hello;
    use crate::network::transport_client::MAX_FRAME_PAYLOAD_BYTES;
    use crate::network::transport_client::read_frame_into;
    use crate::network::transport_client::write_frame;
    use std::net::TcpListener;
    use std::sync::Arc;
    use std::sync::mpsc;
    use std::thread;
    use std::time::Duration;

    #[test]
    fn gateway_connection_allows_multiple_sessions_without_persisting_authorized_phase() {
        let connection = GatewayConnection::new(TransportEndpoint::new("127.0.0.1:1"));

        connection.begin_auth().expect("first auth should begin");
        connection.finish_auth().expect("first auth should finish");
        assert_eq!(connection.phase_name(), "idle");

        connection
            .begin_session_open()
            .expect("first open should begin");
        connection
            .finish_session_open(101)
            .expect("first open should finish");

        connection
            .begin_auth()
            .expect("second auth should begin even with an active session");
        connection.finish_auth().expect("second auth should finish");
        connection
            .begin_session_open()
            .expect("second open should begin");
        connection
            .finish_session_open(202)
            .expect("second open should finish");

        assert_eq!(connection.phase_name(), "idle");
        assert!(connection.is_session_active(101));
        assert!(connection.is_session_active(202));

        connection.clear_session(101);

        assert!(!connection.is_session_active(101));
        assert!(connection.is_session_active(202));
    }

    #[test]
    fn gateway_connection_only_becomes_idle_closeable_after_sessions_and_grants_clear() {
        let connection = GatewayConnection::new(TransportEndpoint::new("127.0.0.1:1"));

        assert!(connection.should_close_after_session_close());

        connection.begin_auth().expect("auth should begin");
        assert!(!connection.should_close_after_session_close());
        connection.fail_auth();
        assert!(connection.should_close_after_session_close());

        connection
            .register_auth_grant(31, "A1b2C3d4E5f6G7h8".to_string())
            .expect("grant should register");
        assert_eq!(connection.auth_grant_count(), 1);
        assert!(!connection.should_close_after_session_close());
        assert!(connection.consume_auth_grant(31));
        assert!(connection.should_close_after_session_close());

        connection
            .begin_session_open()
            .expect("session open should begin");
        assert!(!connection.should_close_after_session_close());
        connection.fail_session_open();
        assert!(connection.should_close_after_session_close());

        connection
            .begin_session_open()
            .expect("second session open should begin");
        connection
            .finish_session_open(501)
            .expect("second session open should finish");
        assert!(!connection.should_close_after_session_close());
        connection.clear_session(501);
        assert!(connection.should_close_after_session_close());
    }

    #[test]
    fn gateway_connection_pairs_responses_by_request_id_under_concurrency() {
        let listener = TcpListener::bind("127.0.0.1:0").expect("bind should succeed");
        let address = listener.local_addr().expect("local_addr should succeed");

        let server = thread::spawn(move || {
            let (mut stream, _) = listener.accept().expect("accept should succeed");
            expect_client_hello(&mut stream);
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
            let (mut stream, _) = listener.accept().expect("accept should succeed");
            expect_client_hello(&mut stream);
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
            let (mut stream, _) = listener.accept().expect("accept should succeed");
            expect_client_hello(&mut stream);
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

    #[test]
    fn gateway_connection_calls_disconnect_handler_when_transport_breaks() {
        let listener = TcpListener::bind("127.0.0.1:0").expect("bind should succeed");
        let address = listener.local_addr().expect("local_addr should succeed");

        let server = thread::spawn(move || {
            let (mut stream, _) = listener.accept().expect("accept should succeed");
            expect_client_hello(&mut stream);
            drop(stream);
        });

        let connection = GatewayConnection::new(TransportEndpoint::new(address.to_string()));
        let (disconnect_tx, disconnect_rx) = mpsc::channel();
        connection.set_disconnect_handler(Some(Arc::new(move || {
            let _ = disconnect_tx.send(());
        })));
        connection.connect().expect("connect should succeed");

        disconnect_rx
            .recv_timeout(Duration::from_millis(200))
            .expect("disconnect handler should be called");

        server.join().expect("server should join");
    }

    #[test]
    fn gateway_connection_sends_periodic_conn_heartbeat() {
        let listener = TcpListener::bind("127.0.0.1:0").expect("bind should succeed");
        let address = listener.local_addr().expect("local_addr should succeed");

        let server = thread::spawn(move || {
            let (mut stream, _) = listener.accept().expect("accept should succeed");
            expect_client_hello(&mut stream);
            let mut buffer = vec![0u8; MAX_FRAME_PAYLOAD_BYTES];
            let request = read_frame_into(&mut stream, &mut buffer)
                .expect("read heartbeat should succeed")
                .to_vec();
            let header =
                crate::network::parse_request_header(&request).expect("parse heartbeat header");
            assert_eq!(header.op_code, ClientOperationCode::ConnHeartbeat);
            assert!(request[crate::network::HEADER_SIZE..].is_empty());

            let response = ProtocolHeader {
                protocol_version: PROTOCOL_VERSION,
                header_len: crate::network::HEADER_SIZE as u8,
                op_code: ClientOperationCode::ConnHeartbeat,
                flags: FLAG_RESPONSE,
                status_code: ProtocolStatusCode::Ok,
                reserved: 0,
                request_id: header.request_id,
                session_id: 0,
            }
            .encode(&[]);
            write_frame(&mut stream, &response).expect("write heartbeat response should succeed");
        });

        let connection = GatewayConnection::new(TransportEndpoint::new(address.to_string()));
        connection.connect().expect("connect should succeed");
        thread::sleep(Duration::from_millis(120));
        let _ = connection.close();

        server.join().expect("server should join");
    }

    #[test]
    fn gateway_connection_disconnects_when_conn_heartbeat_times_out() {
        let listener = TcpListener::bind("127.0.0.1:0").expect("bind should succeed");
        let address = listener.local_addr().expect("local_addr should succeed");

        let server = thread::spawn(move || {
            let (mut stream, _) = listener.accept().expect("accept should succeed");
            expect_client_hello(&mut stream);
            let mut buffer = vec![0u8; MAX_FRAME_PAYLOAD_BYTES];
            let request = read_frame_into(&mut stream, &mut buffer)
                .expect("read heartbeat should succeed")
                .to_vec();
            let header =
                crate::network::parse_request_header(&request).expect("parse heartbeat header");
            assert_eq!(header.op_code, ClientOperationCode::ConnHeartbeat);
            thread::sleep(Duration::from_millis(300));
        });

        let connection = GatewayConnection::new(TransportEndpoint::new(address.to_string()));
        let (disconnect_tx, disconnect_rx) = mpsc::channel();
        connection.set_disconnect_handler(Some(Arc::new(move || {
            let _ = disconnect_tx.send(());
        })));
        connection.connect().expect("connect should succeed");

        disconnect_rx
            .recv_timeout(Duration::from_millis(500))
            .expect("disconnect handler should fire after heartbeat timeout");
        assert!(!connection.is_connected());

        server.join().expect("server should join");
    }

    #[test]
    fn gateway_connection_emits_session_close_notice_and_clears_session() {
        let connection = GatewayConnection::new(TransportEndpoint::new("127.0.0.1:1"));
        connection
            .begin_session_open()
            .expect("begin session open should succeed");
        connection
            .finish_session_open(77)
            .expect("finish session open should succeed");
        let (notice_tx, notice_rx) = mpsc::channel();
        connection.set_session_notice_handler(Some(Arc::new(move |notice| {
            let _ = notice_tx.send((notice.session_id, notice.reason_code));
        })));

        let payload = ProtocolHeader {
            protocol_version: PROTOCOL_VERSION,
            header_len: crate::network::HEADER_SIZE as u8,
            op_code: ClientOperationCode::SessionCloseNotice,
            flags: FLAG_NOTICE,
            status_code: ProtocolStatusCode::Ok,
            reserved: 0,
            request_id: 0,
            session_id: 77,
        }
        .encode(&1u16.to_be_bytes());
        connection
            .dispatch_response(payload)
            .expect("notice dispatch should succeed");

        assert_eq!(
            notice_rx
                .recv_timeout(Duration::from_millis(50))
                .expect("notice handler should fire"),
            (77, 1)
        );
        assert!(!connection.is_session_active(77));
    }
}
