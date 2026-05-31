use std::cmp::Ordering;
use std::collections::BTreeMap;
use std::sync::Mutex;

use backend_rust::BackendContext;
use network_core::client::SessionMetadata;

use crate::state::disk_runtime::DiskRuntimeStore;
use crate::state::network_client::NetworkClientState;
use crate::state::network_client::NetworkDiskKey;
use crate::state::network_client::OpenedNetworkDiskSession;

use super::super::NETWORK_AUTH_FAILED_REASON;
use super::super::NETWORK_AUTH_MISMATCH_REASON;
use super::super::NETWORK_BACKEND_CONFLICT_REASON;
use super::super::NETWORK_CONNECTION_UNAVAILABLE_REASON;
use super::super::NETWORK_METADATA_FAILED_REASON;
use super::super::NETWORK_OPEN_FAILED_REASON;
use super::super::cleanup;
use super::super::gateway_ops;
use super::super::lock_network_client;

#[derive(Debug, Clone)]
struct NetworkRescanTask {
    local_disk_id: String,
    server_addr: String,
    remote_disk_id: String,
    auth_material: String,
    was_mounted: bool,
}

#[derive(Debug, Clone)]
pub(crate) struct NetworkRescanServerPlan {
    server_addr: String,
    tasks: Vec<NetworkRescanTask>,
}

#[derive(Debug, Clone)]
pub(crate) struct NetworkRescanServerResolution {
    server_addr: String,
    actions: Vec<NetworkResolvedAction>,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
enum CandidateSource {
    Existing,
    Fresh,
}

#[derive(Debug, Clone)]
struct PendingSessionCleanup {
    opened_session: OpenedNetworkDiskSession,
    remove_from_state: bool,
}

#[derive(Debug, Clone)]
struct NetworkRescanCandidate {
    task: NetworkRescanTask,
    opened_session: OpenedNetworkDiskSession,
    source: CandidateSource,
    stale_opened_session: Option<OpenedNetworkDiskSession>,
}

#[derive(Debug, Clone)]
struct NetworkRescanFailure {
    task: NetworkRescanTask,
    reason: &'static str,
    stale_opened_session: Option<OpenedNetworkDiskSession>,
}

#[derive(Debug, Clone)]
enum NetworkRescanObservation {
    Candidate(NetworkRescanCandidate),
    Failure(NetworkRescanFailure),
}

#[derive(Debug, Clone)]
enum NetworkRescanCommitResult {
    RefreshMounted {
        metadata: SessionMetadata,
    },
    SetUnmounted {
        metadata: SessionMetadata,
        session_to_insert: Option<OpenedNetworkDiskSession>,
    },
    Invalid {
        reason: &'static str,
    },
}

#[derive(Debug, Clone)]
struct NetworkResolvedAction {
    task: NetworkRescanTask,
    result: NetworkRescanCommitResult,
    sessions_to_cleanup: Vec<PendingSessionCleanup>,
}

impl NetworkRescanTask {
    fn key(&self) -> NetworkDiskKey {
        NetworkDiskKey::new(self.server_addr.clone(), self.remote_disk_id.clone())
    }
}

pub fn rescan_network_runtimes(
    backend: &BackendContext,
    runtime_store: &mut DiskRuntimeStore,
    network_client_mutex: &Mutex<NetworkClientState>,
) {
    let plans = collect_network_rescan_plans(runtime_store);

    for plan in plans {
        let resolution = resolve_network_rescan_plan(network_client_mutex, &plan);
        commit_network_rescan_resolution(backend, runtime_store, network_client_mutex, resolution);
    }
}

pub(crate) fn collect_network_rescan_plans(
    runtime_store: &DiskRuntimeStore,
) -> Vec<NetworkRescanServerPlan> {
    let tasks = collect_network_rescan_tasks(runtime_store);
    group_rescan_tasks_by_server(tasks)
        .into_iter()
        .map(|(server_addr, tasks)| NetworkRescanServerPlan { server_addr, tasks })
        .collect()
}

pub(crate) fn resolve_network_rescan_plan(
    network_client_mutex: &Mutex<NetworkClientState>,
    plan: &NetworkRescanServerPlan,
) -> NetworkRescanServerResolution {
    let observations = collect_server_observations(network_client_mutex, &plan.tasks);
    let actions = resolve_server_actions(observations);
    NetworkRescanServerResolution {
        server_addr: plan.server_addr.clone(),
        actions,
    }
}

pub(crate) fn commit_network_rescan_resolution(
    backend: &BackendContext,
    runtime_store: &mut DiskRuntimeStore,
    network_client_mutex: &Mutex<NetworkClientState>,
    resolution: NetworkRescanServerResolution,
) {
    commit_server_actions(
        backend,
        runtime_store,
        network_client_mutex,
        &resolution.server_addr,
        resolution.actions,
    );
}

fn collect_network_rescan_tasks(runtime_store: &DiskRuntimeStore) -> Vec<NetworkRescanTask> {
    let mut tasks = runtime_store
        .runtimes()
        .filter_map(|runtime| {
            if !runtime.is_network() {
                return None;
            }

            Some(NetworkRescanTask {
                local_disk_id: runtime.local_disk_id().to_string(),
                server_addr: runtime.server_addr()?.to_string(),
                remote_disk_id: runtime.remote_disk_id()?.to_string(),
                auth_material: runtime.auth_material()?.to_string(),
                was_mounted: runtime.mounted_target_id().is_some(),
            })
        })
        .collect::<Vec<_>>();
    tasks.sort_by(|left, right| compare_local_disk_id(&left.local_disk_id, &right.local_disk_id));
    tasks
}

fn group_rescan_tasks_by_server(
    tasks: Vec<NetworkRescanTask>,
) -> BTreeMap<String, Vec<NetworkRescanTask>> {
    let mut tasks_by_server = BTreeMap::<String, Vec<NetworkRescanTask>>::new();
    for task in tasks {
        tasks_by_server
            .entry(task.server_addr.clone())
            .or_default()
            .push(task);
    }
    tasks_by_server
}

fn collect_server_observations(
    network_client_mutex: &Mutex<NetworkClientState>,
    tasks: &[NetworkRescanTask],
) -> Vec<NetworkRescanObservation> {
    tasks
        .iter()
        .cloned()
        .map(|task| collect_task_observation(network_client_mutex, task))
        .collect()
}

fn collect_task_observation(
    network_client_mutex: &Mutex<NetworkClientState>,
    task: NetworkRescanTask,
) -> NetworkRescanObservation {
    let key = task.key();
    let existing_opened_session = {
        let network_client = lock_network_client(network_client_mutex);
        network_client.opened_session(&key)
    };

    if let Some(opened_session) = existing_opened_session.clone() {
        if opened_session.session.ensure_usable().is_ok() {
            return NetworkRescanObservation::Candidate(NetworkRescanCandidate {
                task,
                opened_session,
                source: CandidateSource::Existing,
                stale_opened_session: None,
            });
        }
    }

    let stale_opened_session =
        existing_opened_session.filter(|session| session.session.ensure_usable().is_err());

    let connection = match gateway_ops::acquire_connection(network_client_mutex, &task.server_addr)
    {
        Ok(connection) => connection,
        Err(_) => {
            return NetworkRescanObservation::Failure(NetworkRescanFailure {
                task,
                reason: NETWORK_CONNECTION_UNAVAILABLE_REASON,
                stale_opened_session,
            });
        }
    };

    let auth = match gateway_ops::authenticate(connection.clone(), &task.auth_material) {
        Ok(auth) => auth,
        Err(_) => {
            return NetworkRescanObservation::Failure(NetworkRescanFailure {
                task,
                reason: NETWORK_AUTH_FAILED_REASON,
                stale_opened_session,
            });
        }
    };

    if auth.disk_id() != task.remote_disk_id {
        let _ = connection.discard_auth_grant(auth.auth_id());
        return NetworkRescanObservation::Failure(NetworkRescanFailure {
            task,
            reason: NETWORK_AUTH_MISMATCH_REASON,
            stale_opened_session,
        });
    }

    let session = match gateway_ops::open_session(connection.clone(), &auth) {
        Ok(session) => session,
        Err(_) => {
            return NetworkRescanObservation::Failure(NetworkRescanFailure {
                task,
                reason: NETWORK_OPEN_FAILED_REASON,
                stale_opened_session,
            });
        }
    };

    let metadata = match gateway_ops::describe_session(connection, session.session_id()) {
        Ok(metadata) => metadata,
        Err(_) => {
            let _ = cleanup::close_session_for_cleanup(&session);
            let mut network_client = lock_network_client(network_client_mutex);
            network_client.release_connection_after_session_close(&task.server_addr);
            return NetworkRescanObservation::Failure(NetworkRescanFailure {
                task,
                reason: NETWORK_METADATA_FAILED_REASON,
                stale_opened_session,
            });
        }
    };

    NetworkRescanObservation::Candidate(NetworkRescanCandidate {
        task: task.clone(),
        opened_session: OpenedNetworkDiskSession {
            key,
            session,
            metadata,
        },
        source: CandidateSource::Fresh,
        stale_opened_session,
    })
}

fn resolve_server_actions(
    observations: Vec<NetworkRescanObservation>,
) -> Vec<NetworkResolvedAction> {
    let mut failure_actions = Vec::new();
    let mut candidate_groups = BTreeMap::<[u8; 16], Vec<NetworkRescanCandidate>>::new();

    for observation in observations {
        match observation {
            NetworkRescanObservation::Failure(failure) => {
                failure_actions.push(build_failure_action(failure));
            }
            NetworkRescanObservation::Candidate(candidate) => {
                candidate_groups
                    .entry(candidate.opened_session.metadata.backend_id)
                    .or_default()
                    .push(candidate);
            }
        }
    }

    let mut actions = failure_actions;
    for (_, mut candidates) in candidate_groups {
        candidates.sort_by(|left, right| {
            compare_local_disk_id(&left.task.local_disk_id, &right.task.local_disk_id)
        });

        if candidates.len() == 1 {
            actions.push(build_available_action(
                candidates.pop().expect("single candidate should exist"),
            ));
            continue;
        }

        let winner_local_disk_id = candidates
            .iter()
            .filter(|candidate| candidate.opened_session.metadata.read_only)
            .min_by(|left, right| {
                compare_local_disk_id(&left.task.local_disk_id, &right.task.local_disk_id)
            })
            .map(|candidate| candidate.task.local_disk_id.clone());

        for candidate in candidates {
            let is_winner = winner_local_disk_id
                .as_ref()
                .is_some_and(|local_disk_id| local_disk_id == &candidate.task.local_disk_id);
            if is_winner {
                actions.push(build_available_action(candidate));
            } else {
                actions.push(build_backend_conflict_action(candidate));
            }
        }
    }

    actions.sort_by(|left, right| {
        compare_local_disk_id(&left.task.local_disk_id, &right.task.local_disk_id)
    });
    actions
}

fn build_available_action(candidate: NetworkRescanCandidate) -> NetworkResolvedAction {
    let mut sessions_to_cleanup = Vec::new();
    if let Some(stale_opened_session) = candidate.stale_opened_session {
        sessions_to_cleanup.push(PendingSessionCleanup {
            opened_session: stale_opened_session,
            remove_from_state: true,
        });
    }

    let result = match candidate.source {
        CandidateSource::Existing if candidate.task.was_mounted => {
            NetworkRescanCommitResult::RefreshMounted {
                metadata: candidate.opened_session.metadata,
            }
        }
        CandidateSource::Existing => NetworkRescanCommitResult::SetUnmounted {
            metadata: candidate.opened_session.metadata,
            session_to_insert: None,
        },
        CandidateSource::Fresh => NetworkRescanCommitResult::SetUnmounted {
            metadata: candidate.opened_session.metadata,
            session_to_insert: Some(candidate.opened_session),
        },
    };

    NetworkResolvedAction {
        task: candidate.task,
        result,
        sessions_to_cleanup,
    }
}

fn build_failure_action(failure: NetworkRescanFailure) -> NetworkResolvedAction {
    let mut sessions_to_cleanup = Vec::new();
    if let Some(stale_opened_session) = failure.stale_opened_session {
        sessions_to_cleanup.push(PendingSessionCleanup {
            opened_session: stale_opened_session,
            remove_from_state: true,
        });
    }

    NetworkResolvedAction {
        task: failure.task,
        result: NetworkRescanCommitResult::Invalid {
            reason: failure.reason,
        },
        sessions_to_cleanup,
    }
}

fn build_backend_conflict_action(candidate: NetworkRescanCandidate) -> NetworkResolvedAction {
    let mut sessions_to_cleanup = Vec::new();
    if let Some(stale_opened_session) = candidate.stale_opened_session {
        sessions_to_cleanup.push(PendingSessionCleanup {
            opened_session: stale_opened_session,
            remove_from_state: true,
        });
    }
    sessions_to_cleanup.push(PendingSessionCleanup {
        opened_session: candidate.opened_session,
        remove_from_state: candidate.source == CandidateSource::Existing,
    });

    NetworkResolvedAction {
        task: candidate.task,
        result: NetworkRescanCommitResult::Invalid {
            reason: NETWORK_BACKEND_CONFLICT_REASON,
        },
        sessions_to_cleanup,
    }
}

fn commit_server_actions(
    backend: &BackendContext,
    runtime_store: &mut DiskRuntimeStore,
    network_client_mutex: &Mutex<NetworkClientState>,
    server_addr: &str,
    actions: Vec<NetworkResolvedAction>,
) {
    let (sessions_to_close, sessions_to_insert) = {
        let mut network_client = lock_network_client(network_client_mutex);
        let mut sessions_to_close = Vec::new();
        let mut sessions_to_insert = Vec::new();

        for action in &actions {
            for pending_cleanup in &action.sessions_to_cleanup {
                if pending_cleanup.remove_from_state {
                    let opened_session = network_client
                        .remove_opened_session(&pending_cleanup.opened_session.key)
                        .unwrap_or_else(|| pending_cleanup.opened_session.clone());
                    sessions_to_close.push(opened_session.session);
                } else {
                    sessions_to_close.push(pending_cleanup.opened_session.session.clone());
                }
            }

            if let NetworkRescanCommitResult::SetUnmounted {
                session_to_insert: Some(opened_session),
                ..
            } = &action.result
            {
                sessions_to_insert.push(opened_session.clone());
            }
        }

        (sessions_to_close, sessions_to_insert)
    };

    let closed_any_session = !sessions_to_close.is_empty();
    for session in sessions_to_close {
        let _ = cleanup::close_session_for_cleanup(&session);
    }

    {
        let mut network_client = lock_network_client(network_client_mutex);
        for opened_session in sessions_to_insert {
            network_client.insert_opened_session(opened_session);
        }
        if closed_any_session {
            network_client.release_connection_after_session_close(server_addr);
        }
    }

    for action in actions {
        apply_runtime_action(backend, runtime_store, action);
    }
}

fn apply_runtime_action(
    backend: &BackendContext,
    runtime_store: &mut DiskRuntimeStore,
    action: NetworkResolvedAction,
) {
    match action.result {
        NetworkRescanCommitResult::RefreshMounted { metadata } => {
            cleanup::refresh_network_runtime(runtime_store, &action.task.local_disk_id, metadata);
        }
        NetworkRescanCommitResult::SetUnmounted { metadata, .. } => {
            cleanup::set_network_runtime_unmounted(
                backend,
                runtime_store,
                &action.task.local_disk_id,
                metadata,
            );
        }
        NetworkRescanCommitResult::Invalid { reason } => {
            cleanup::set_network_runtime_invalid(
                backend,
                runtime_store,
                &action.task.local_disk_id,
                reason,
            );
        }
    }
}

fn compare_local_disk_id(left: &str, right: &str) -> Ordering {
    match (
        parse_local_disk_id_number(left),
        parse_local_disk_id_number(right),
    ) {
        (Some(left_number), Some(right_number)) => {
            left_number.cmp(&right_number).then_with(|| left.cmp(right))
        }
        _ => left.cmp(right),
    }
}

fn parse_local_disk_id_number(local_disk_id: &str) -> Option<u64> {
    local_disk_id.strip_prefix("disk-")?.parse::<u64>().ok()
}

#[cfg(test)]
mod tests {
    use std::collections::HashMap;
    use std::net::TcpListener;
    use std::sync::Arc;
    use std::sync::Mutex;
    use std::sync::atomic::AtomicBool;
    use std::sync::atomic::Ordering;
    use std::thread;
    use std::time::Duration;

    use backend_rust::BackendContext;
    use network_core::client::DiskSession;
    use network_core::client::GatewayConnection;
    use network_core::client::SessionMetadata;
    use network_core::protocol::ClientOperationCode;
    use network_core::protocol::FLAG_NOTICE;
    use network_core::protocol::FLAG_RESPONSE;
    use network_core::protocol::HEADER_SIZE;
    use network_core::protocol::PROTOCOL_VERSION;
    use network_core::protocol::ProtocolHeader;
    use network_core::protocol::ProtocolStatusCode;
    use network_core::protocol::SessionCloseNotice;
    use network_core::protocol::parse_header;
    use network_core::protocol::parse_request_header;
    use network_core::test_support::expect_client_hello;
    use network_core::test_support::stage_connection;
    use network_core::transport::MAX_FRAME_PAYLOAD_BYTES;
    use network_core::transport::TransportEndpoint;
    use network_core::transport::read_frame_into;
    use network_core::transport::write_frame;

    use super::rescan_network_runtimes;
    use crate::network::NETWORK_BACKEND_CONFLICT_REASON;
    use crate::state::disk_runtime::DiskRuntime;
    use crate::state::disk_runtime::DiskRuntimeStatus;
    use crate::state::disk_runtime::DiskRuntimeStore;
    use crate::state::network_client::NetworkClientState;
    use crate::state::network_client::NetworkDiskKey;
    use crate::state::network_client::OpenedNetworkDiskSession;

    #[derive(Debug, Clone)]
    struct ScriptedDisk {
        disk_id: String,
        auth_id: u64,
        session_id: u64,
        metadata: SessionMetadata,
    }

    struct ConnectedSessionHarness {
        server_addr: String,
        connection: Arc<GatewayConnection>,
        stop: Arc<AtomicBool>,
        server: thread::JoinHandle<()>,
    }

    struct ScriptedGatewayServer {
        server_addr: String,
        closed_sessions: Arc<Mutex<Vec<u64>>>,
        stop: Arc<AtomicBool>,
        server: thread::JoinHandle<()>,
    }

    fn sample_metadata() -> SessionMetadata {
        SessionMetadata {
            disk_size_bytes: 4096,
            read_only: false,
            backend_id: [0; 16],
        }
    }

    impl ConnectedSessionHarness {
        fn new(session_id: u64) -> Self {
            let listener = TcpListener::bind("127.0.0.1:0").expect("listener should bind");
            let server_addr = listener
                .local_addr()
                .expect("local addr should exist")
                .to_string();
            let stop = Arc::new(AtomicBool::new(false));
            let stop_for_server = Arc::clone(&stop);
            let server = thread::spawn(move || {
                let (mut stream, _) = listener.accept().expect("accept should succeed");
                expect_client_hello(&mut stream);
                while !stop_for_server.load(Ordering::SeqCst) {
                    thread::sleep(Duration::from_millis(10));
                }
            });

            let connection =
                stage_connection(TransportEndpoint::new(server_addr.clone()), session_id);
            connection.connect().expect("connect should succeed");

            Self {
                server_addr,
                connection,
                stop,
                server,
            }
        }

        fn session(&self, session_id: u64) -> DiskSession {
            DiskSession::new(Arc::clone(&self.connection), session_id)
                .expect("session should build")
        }

        fn shutdown(self) {
            self.stop.store(true, Ordering::SeqCst);
            let _ = self.connection.close();
            let _ = self.server.join();
        }
    }

    impl ScriptedGatewayServer {
        fn new(disks: Vec<ScriptedDisk>) -> Self {
            let listener = TcpListener::bind("127.0.0.1:0").expect("listener should bind");
            let server_addr = listener
                .local_addr()
                .expect("server addr should exist")
                .to_string();
            let closed_sessions = Arc::new(Mutex::new(Vec::new()));
            let closed_sessions_for_server = Arc::clone(&closed_sessions);
            let stop = Arc::new(AtomicBool::new(false));
            let stop_for_server = Arc::clone(&stop);
            let disks_by_id = disks
                .iter()
                .cloned()
                .map(|disk| (disk.disk_id.clone(), disk))
                .collect::<HashMap<_, _>>();
            let disks_by_auth_id = disks
                .iter()
                .cloned()
                .map(|disk| (disk.auth_id, disk))
                .collect::<HashMap<_, _>>();
            let disks_by_session_id = disks
                .into_iter()
                .map(|disk| (disk.session_id, disk))
                .collect::<HashMap<_, _>>();

            let server = thread::spawn(move || {
                let (mut stream, _) = listener.accept().expect("accept should succeed");
                expect_client_hello(&mut stream);
                let mut buffer = vec![0u8; MAX_FRAME_PAYLOAD_BYTES];

                while !stop_for_server.load(Ordering::SeqCst) {
                    let Ok(frame) = read_frame_into(&mut stream, &mut buffer) else {
                        break;
                    };
                    let payload = frame.to_vec();
                    let header = parse_header(&payload).expect("request header should parse");

                    match header.op_code {
                        ClientOperationCode::AuthStart => {
                            let request =
                                parse_request_header(&payload).expect("auth start should parse");
                            let disk_id = std::str::from_utf8(&payload[HEADER_SIZE..])
                                .expect("disk id should be utf8")
                                .to_string();
                            let disk = disks_by_id
                                .get(&disk_id)
                                .expect("disk should exist for auth start");
                            let mut body = Vec::new();
                            body.push(1);
                            body.extend_from_slice(&30u16.to_be_bytes());
                            body.extend_from_slice(&[7u8; 16]);
                            body.extend_from_slice(&(disk.disk_id.len() as u16).to_be_bytes());
                            body.extend_from_slice(disk.disk_id.as_bytes());
                            let response = ProtocolHeader {
                                protocol_version: PROTOCOL_VERSION,
                                header_len: HEADER_SIZE as u8,
                                op_code: ClientOperationCode::AuthStart,
                                flags: FLAG_RESPONSE,
                                status_code: ProtocolStatusCode::Ok,
                                reserved: 0,
                                request_id: request.request_id,
                                session_id: 0,
                            }
                            .encode(&body);
                            write_frame(&mut stream, &response)
                                .expect("auth start response should be writable");
                        }
                        ClientOperationCode::AuthFinish => {
                            let request =
                                parse_request_header(&payload).expect("auth finish should parse");
                            let token_len = u16::from_be_bytes(
                                payload[HEADER_SIZE..HEADER_SIZE + 2]
                                    .try_into()
                                    .expect("token len slice should exist"),
                            ) as usize;
                            let token_start = HEADER_SIZE + 2;
                            let token_end = token_start + token_len;
                            let disk_id = std::str::from_utf8(&payload[token_start..token_end])
                                .expect("challenge token should be utf8")
                                .to_string();
                            let disk = disks_by_id
                                .get(&disk_id)
                                .expect("disk should exist for auth finish");
                            let response = ProtocolHeader {
                                protocol_version: PROTOCOL_VERSION,
                                header_len: HEADER_SIZE as u8,
                                op_code: ClientOperationCode::AuthFinish,
                                flags: FLAG_RESPONSE,
                                status_code: ProtocolStatusCode::Ok,
                                reserved: 0,
                                request_id: request.request_id,
                                session_id: 0,
                            }
                            .encode(&disk.auth_id.to_be_bytes());
                            write_frame(&mut stream, &response)
                                .expect("auth finish response should be writable");
                        }
                        ClientOperationCode::SessionOpen => {
                            let request =
                                parse_request_header(&payload).expect("session open should parse");
                            let auth_id = u64::from_be_bytes(
                                payload[HEADER_SIZE..HEADER_SIZE + 8]
                                    .try_into()
                                    .expect("auth id slice should exist"),
                            );
                            let disk = disks_by_auth_id
                                .get(&auth_id)
                                .expect("disk should exist for session open");
                            let response = ProtocolHeader {
                                protocol_version: PROTOCOL_VERSION,
                                header_len: HEADER_SIZE as u8,
                                op_code: ClientOperationCode::SessionOpen,
                                flags: FLAG_RESPONSE,
                                status_code: ProtocolStatusCode::Ok,
                                reserved: 0,
                                request_id: request.request_id,
                                session_id: disk.session_id,
                            }
                            .encode(&[]);
                            write_frame(&mut stream, &response)
                                .expect("session open response should be writable");
                        }
                        ClientOperationCode::SessionDescribe => {
                            let request = parse_request_header(&payload)
                                .expect("session describe should parse");
                            let disk = disks_by_session_id
                                .get(&request.session_id)
                                .expect("disk should exist for describe");
                            let mut body = Vec::new();
                            body.extend_from_slice(&disk.metadata.disk_size_bytes.to_be_bytes());
                            body.extend_from_slice(
                                &(u16::from(disk.metadata.read_only)).to_be_bytes(),
                            );
                            body.extend_from_slice(&0u16.to_be_bytes());
                            body.extend_from_slice(&disk.metadata.backend_id);
                            let response = ProtocolHeader {
                                protocol_version: PROTOCOL_VERSION,
                                header_len: HEADER_SIZE as u8,
                                op_code: ClientOperationCode::SessionDescribe,
                                flags: FLAG_RESPONSE,
                                status_code: ProtocolStatusCode::Ok,
                                reserved: 0,
                                request_id: request.request_id,
                                session_id: disk.session_id,
                            }
                            .encode(&body);
                            write_frame(&mut stream, &response)
                                .expect("session describe response should be writable");
                        }
                        ClientOperationCode::SessionCloseNotice => {
                            assert_eq!(header.flags, FLAG_NOTICE);
                            let notice = SessionCloseNotice::decode_notice(&payload)
                                .expect("session close notice should decode");
                            closed_sessions_for_server
                                .lock()
                                .expect("closed session log should not be poisoned")
                                .push(notice.session_id);
                        }
                        ClientOperationCode::ConnHeartbeat => {
                            let request =
                                parse_request_header(&payload).expect("heartbeat should parse");
                            let response = ProtocolHeader {
                                protocol_version: PROTOCOL_VERSION,
                                header_len: HEADER_SIZE as u8,
                                op_code: ClientOperationCode::ConnHeartbeat,
                                flags: FLAG_RESPONSE,
                                status_code: ProtocolStatusCode::Ok,
                                reserved: 0,
                                request_id: request.request_id,
                                session_id: 0,
                            }
                            .encode(&[]);
                            write_frame(&mut stream, &response)
                                .expect("heartbeat response should be writable");
                        }
                        other => panic!("unexpected op code: {:?}", other),
                    }
                }
            });

            Self {
                server_addr,
                closed_sessions,
                stop,
                server,
            }
        }

        fn closed_sessions(&self) -> Vec<u64> {
            self.closed_sessions
                .lock()
                .expect("closed session log should not be poisoned")
                .clone()
        }

        fn wait_for_closed_sessions(&self, expected: &[u64]) {
            for _ in 0..50 {
                if self.closed_sessions() == expected {
                    return;
                }
                thread::sleep(Duration::from_millis(10));
            }
            assert_eq!(self.closed_sessions(), expected);
        }

        fn shutdown(self) {
            self.stop.store(true, Ordering::SeqCst);
            let _ = std::net::TcpStream::connect(&self.server_addr);
            let _ = self.server.join();
        }
    }

    #[test]
    fn rescan_network_runtimes_reuses_live_opened_session() {
        let backend = BackendContext::default();
        let harness = ConnectedSessionHarness::new(31);
        let key = NetworkDiskKey::new(&harness.server_addr, "Q1w2E3r4T5y6U7i8");
        let opened_session = OpenedNetworkDiskSession {
            key: key.clone(),
            session: harness.session(31),
            metadata: sample_metadata(),
        };

        let mut network_client = NetworkClientState::default();
        network_client.insert_opened_session(opened_session);
        let network_client_mutex = Mutex::new(network_client);

        let mut runtime_store = DiskRuntimeStore::default();
        runtime_store.insert_runtime(DiskRuntime::new_network(
            "disk-1".to_string(),
            "network-disk".to_string(),
            false,
            harness.server_addr.clone(),
            "Q1w2E3r4T5y6U7i8".to_string(),
            "claim-4".to_string(),
            1,
            false,
            true,
        ));

        rescan_network_runtimes(&backend, &mut runtime_store, &network_client_mutex);

        let runtime = runtime_store
            .find_runtime("disk-1")
            .expect("runtime should exist");
        assert_eq!(runtime.status(), &DiskRuntimeStatus::Unmounted);
        assert_eq!(runtime.capacity_bytes(), 4096);
        assert!(!runtime.source_read_only());
        assert!(!runtime.configured_read_only());

        let network_client = network_client_mutex
            .lock()
            .expect("network client mutex should not be poisoned");
        assert!(network_client.opened_session(&key).is_some());

        harness.shutdown();
    }

    #[test]
    fn rescan_network_runtimes_keeps_mounted_runtime_mounted_when_session_is_live() {
        let backend = BackendContext::default();
        let harness = ConnectedSessionHarness::new(32);
        let key = NetworkDiskKey::new(&harness.server_addr, "Q1w2E3r4T5y6U7i8");
        let opened_session = OpenedNetworkDiskSession {
            key: key.clone(),
            session: harness.session(32),
            metadata: sample_metadata(),
        };

        let mut network_client = NetworkClientState::default();
        network_client.insert_opened_session(opened_session);
        let network_client_mutex = Mutex::new(network_client);

        let mut runtime_store = DiskRuntimeStore::default();
        let mut runtime = DiskRuntime::new_network(
            "disk-1".to_string(),
            "network-disk".to_string(),
            false,
            harness.server_addr.clone(),
            "Q1w2E3r4T5y6U7i8".to_string(),
            "claim-5".to_string(),
            1,
            false,
            true,
        );
        runtime.set_mounted(7);
        runtime_store.insert_runtime(runtime);

        rescan_network_runtimes(&backend, &mut runtime_store, &network_client_mutex);

        let runtime = runtime_store
            .find_runtime("disk-1")
            .expect("runtime should exist");
        assert_eq!(
            runtime.status(),
            &DiskRuntimeStatus::Mounted { target_id: 7 }
        );
        assert_eq!(runtime.capacity_bytes(), 4096);
        assert!(!runtime.source_read_only());
        assert!(!runtime.configured_read_only());

        let network_client = network_client_mutex
            .lock()
            .expect("network client mutex should not be poisoned");
        assert!(network_client.opened_session(&key).is_some());

        harness.shutdown();
    }

    #[test]
    fn rescan_network_runtimes_reopens_stale_mounted_runtime_as_unmounted() {
        let backend = BackendContext::default();
        let server = ScriptedGatewayServer::new(vec![ScriptedDisk {
            disk_id: "A1b2C3d4E5f6G7h8".to_string(),
            auth_id: 81,
            session_id: 91,
            metadata: SessionMetadata {
                disk_size_bytes: 8192,
                read_only: false,
                backend_id: [2; 16],
            },
        }]);

        let stale_connection =
            stage_connection(TransportEndpoint::new(server.server_addr.clone()), 41);
        let stale_session =
            DiskSession::new(Arc::clone(&stale_connection), 41).expect("session should build");
        stale_session
            .close()
            .expect("stale session close should succeed");

        let key = NetworkDiskKey::new(&server.server_addr, "A1b2C3d4E5f6G7h8");
        let mut network_client = NetworkClientState::default();
        network_client.insert_opened_session(OpenedNetworkDiskSession {
            key: key.clone(),
            session: stale_session,
            metadata: sample_metadata(),
        });
        let network_client_mutex = Mutex::new(network_client);

        let mut runtime_store = DiskRuntimeStore::default();
        let mut runtime = DiskRuntime::new_network(
            "disk-1".to_string(),
            "network-disk".to_string(),
            false,
            server.server_addr.clone(),
            "A1b2C3d4E5f6G7h8".to_string(),
            "A1b2C3d4E5f6G7h8abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789ab"
                .to_string(),
            1,
            false,
            false,
        );
        runtime.set_mounted(9);
        runtime_store.insert_runtime(runtime);

        rescan_network_runtimes(&backend, &mut runtime_store, &network_client_mutex);

        let runtime = runtime_store
            .find_runtime("disk-1")
            .expect("runtime should exist");
        assert_eq!(runtime.status(), &DiskRuntimeStatus::Unmounted);
        assert_eq!(runtime.capacity_bytes(), 8192);

        let network_client = network_client_mutex
            .lock()
            .expect("network client mutex should not be poisoned");
        let opened_session = network_client
            .opened_session(&key)
            .expect("fresh opened session should exist");
        assert_eq!(opened_session.session.session_id(), 91);

        server.shutdown();
    }

    #[test]
    fn rescan_network_runtimes_keeps_only_ro_when_backend_conflicts_with_rw() {
        let backend = BackendContext::default();
        let server = ScriptedGatewayServer::new(vec![
            ScriptedDisk {
                disk_id: "A1b2C3d4E5f6G7h8".to_string(),
                auth_id: 101,
                session_id: 201,
                metadata: SessionMetadata {
                    disk_size_bytes: 4096,
                    read_only: false,
                    backend_id: [5; 16],
                },
            },
            ScriptedDisk {
                disk_id: "Z9y8X7w6V5u4T3s2".to_string(),
                auth_id: 102,
                session_id: 202,
                metadata: SessionMetadata {
                    disk_size_bytes: 4096,
                    read_only: true,
                    backend_id: [5; 16],
                },
            },
        ]);
        let network_client_mutex = Mutex::new(NetworkClientState::default());

        let mut runtime_store = DiskRuntimeStore::default();
        runtime_store.insert_runtime(DiskRuntime::new_network(
            "disk-2".to_string(),
            "rw-disk".to_string(),
            false,
            server.server_addr.clone(),
            "A1b2C3d4E5f6G7h8".to_string(),
            "A1b2C3d4E5f6G7h8abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789ab"
                .to_string(),
            1,
            false,
            false,
        ));
        runtime_store.insert_runtime(DiskRuntime::new_network(
            "disk-1".to_string(),
            "ro-disk".to_string(),
            false,
            server.server_addr.clone(),
            "Z9y8X7w6V5u4T3s2".to_string(),
            "Z9y8X7w6V5u4T3s2abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789ab"
                .to_string(),
            1,
            false,
            false,
        ));

        rescan_network_runtimes(&backend, &mut runtime_store, &network_client_mutex);

        let rw_runtime = runtime_store
            .find_runtime("disk-2")
            .expect("rw runtime should exist");
        assert_eq!(
            rw_runtime.status(),
            &DiskRuntimeStatus::Invalid {
                reason: NETWORK_BACKEND_CONFLICT_REASON.to_string(),
            }
        );

        let ro_runtime = runtime_store
            .find_runtime("disk-1")
            .expect("ro runtime should exist");
        assert_eq!(ro_runtime.status(), &DiskRuntimeStatus::Unmounted);
        assert!(ro_runtime.source_read_only());

        let network_client = network_client_mutex
            .lock()
            .expect("network client mutex should not be poisoned");
        assert!(
            network_client
                .opened_session(&NetworkDiskKey::new(
                    &server.server_addr,
                    "A1b2C3d4E5f6G7h8"
                ))
                .is_none()
        );
        assert!(
            network_client
                .opened_session(&NetworkDiskKey::new(
                    &server.server_addr,
                    "Z9y8X7w6V5u4T3s2"
                ))
                .is_some()
        );

        server.wait_for_closed_sessions(&[201]);
        server.shutdown();
    }

    #[test]
    fn rescan_network_runtimes_keeps_smallest_ro_local_disk_id_for_backend_conflict() {
        let backend = BackendContext::default();
        let server = ScriptedGatewayServer::new(vec![
            ScriptedDisk {
                disk_id: "A1b2C3d4E5f6G7h8".to_string(),
                auth_id: 111,
                session_id: 211,
                metadata: SessionMetadata {
                    disk_size_bytes: 4096,
                    read_only: true,
                    backend_id: [6; 16],
                },
            },
            ScriptedDisk {
                disk_id: "Z9y8X7w6V5u4T3s2".to_string(),
                auth_id: 112,
                session_id: 212,
                metadata: SessionMetadata {
                    disk_size_bytes: 4096,
                    read_only: true,
                    backend_id: [6; 16],
                },
            },
        ]);
        let network_client_mutex = Mutex::new(NetworkClientState::default());

        let mut runtime_store = DiskRuntimeStore::default();
        runtime_store.insert_runtime(DiskRuntime::new_network(
            "disk-10".to_string(),
            "ro-10".to_string(),
            false,
            server.server_addr.clone(),
            "A1b2C3d4E5f6G7h8".to_string(),
            "A1b2C3d4E5f6G7h8abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789ab"
                .to_string(),
            1,
            false,
            false,
        ));
        runtime_store.insert_runtime(DiskRuntime::new_network(
            "disk-2".to_string(),
            "ro-2".to_string(),
            false,
            server.server_addr.clone(),
            "Z9y8X7w6V5u4T3s2".to_string(),
            "Z9y8X7w6V5u4T3s2abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789ab"
                .to_string(),
            1,
            false,
            false,
        ));

        rescan_network_runtimes(&backend, &mut runtime_store, &network_client_mutex);

        let losing_runtime = runtime_store
            .find_runtime("disk-10")
            .expect("disk-10 runtime should exist");
        assert_eq!(
            losing_runtime.status(),
            &DiskRuntimeStatus::Invalid {
                reason: NETWORK_BACKEND_CONFLICT_REASON.to_string(),
            }
        );

        let winner_runtime = runtime_store
            .find_runtime("disk-2")
            .expect("disk-2 runtime should exist");
        assert_eq!(winner_runtime.status(), &DiskRuntimeStatus::Unmounted);
        assert!(winner_runtime.source_read_only());

        let network_client = network_client_mutex
            .lock()
            .expect("network client mutex should not be poisoned");
        assert!(
            network_client
                .opened_session(&NetworkDiskKey::new(
                    &server.server_addr,
                    "A1b2C3d4E5f6G7h8"
                ))
                .is_none()
        );
        assert!(
            network_client
                .opened_session(&NetworkDiskKey::new(
                    &server.server_addr,
                    "Z9y8X7w6V5u4T3s2"
                ))
                .is_some()
        );

        server.wait_for_closed_sessions(&[211]);
        server.shutdown();
    }
}
