use std::sync::Arc;
use std::sync::Mutex;

use backend_rust::BackendContext;
use network_core::client::DiskSession;
use network_core::client::NetworkClientError;
use network_core::client::SessionMetadata;

use crate::api_error::ApiError;
use crate::backend::persistence_service;
use crate::state::disk_runtime::DiskRuntime;
use crate::state::disk_runtime::DiskRuntimeStore;
use crate::state::network_client::NetworkClientState;
use crate::state::network_client::NetworkCreateDraft;
use crate::state::network_client::NetworkDiskKey;
use crate::state::network_client::NetworkDraftItem;
use crate::state::network_client::OpenedNetworkDiskSession;

use super::cleanup;
use super::gateway_ops;
use super::lock_network_client;
use super::uniqueness;
use super::validation;

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct NetworkDraftItemSnapshot {
    pub disk_name: String,
    pub server_addr: String,
    pub remote_disk_id: String,
    pub capacity_bytes: u64,
    pub read_only: bool,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct NetworkCreateDraftSnapshot {
    pub draft_id: String,
    pub server_addr: String,
    pub items: Vec<NetworkDraftItemSnapshot>,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct CreateNetworkDraftRequest {
    pub server_addr: String,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct AddNetworkDraftItemRequest {
    pub draft_id: String,
    pub disk_name: String,
    pub claim_code: String,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct RemoveNetworkDraftItemRequest {
    pub draft_id: String,
    pub remote_disk_id: String,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct SubmitNetworkDraftRequest {
    pub draft_id: String,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct DisposeNetworkDraftRequest {
    pub draft_id: String,
}

pub fn test_connection(
    network_client_mutex: &Mutex<NetworkClientState>,
    server_addr: &str,
) -> Result<(), ApiError> {
    let server_addr = validation::validate_server_addr(server_addr)?;
    gateway_ops::acquire_connection(network_client_mutex, server_addr)
        .map(|_| ())
        .map_err(map_connect_error)
}

pub fn create_network_draft(
    network_client_mutex: &Mutex<NetworkClientState>,
    request: CreateNetworkDraftRequest,
) -> Result<NetworkCreateDraftSnapshot, ApiError> {
    let server_addr = validation::validate_server_addr(&request.server_addr)?.to_string();
    let connection = gateway_ops::acquire_connection(network_client_mutex, &server_addr)
        .map_err(map_connect_error)?;
    let mut network_client = lock_network_client(network_client_mutex);
    let draft_id = network_client.allocate_draft_id();
    let draft = NetworkCreateDraft::new(draft_id.clone(), server_addr, connection);
    let snapshot = map_draft_snapshot(&draft);
    network_client.insert_draft(draft);
    Ok(snapshot)
}

pub fn add_network_draft_item(
    runtime_store: &DiskRuntimeStore,
    network_client_mutex: &Mutex<NetworkClientState>,
    request: AddNetworkDraftItemRequest,
) -> Result<NetworkCreateDraftSnapshot, ApiError> {
    let disk_name = validation::validate_disk_name(&request.disk_name)?.to_string();
    let (server_addr, connection) = {
        let network_client = lock_network_client(network_client_mutex);
        let draft = network_client
            .draft(&request.draft_id)
            .ok_or_else(|| validation::draft_not_found_error(&request.draft_id))?;
        (draft.server_addr.clone(), Arc::clone(&draft.connection))
    };

    let auth = gateway_ops::authenticate(Arc::clone(&connection), &request.claim_code)
        .map_err(map_auth_error)?;
    let key = NetworkDiskKey::new(server_addr.clone(), auth.disk_id().to_string());

    {
        let network_client = lock_network_client(network_client_mutex);
        if let Err(error) =
            uniqueness::ensure_unique_network_key(runtime_store, &network_client, &key, None)
        {
            let _ = connection.discard_auth_grant(auth.auth_id());
            return Err(error);
        }
    }

    let session =
        gateway_ops::open_session(Arc::clone(&connection), &auth).map_err(map_open_error)?;
    let metadata =
        match gateway_ops::describe_session(Arc::clone(&connection), session.session_id()) {
            Ok(metadata) => metadata,
            Err(error) => {
                let _ = cleanup::close_session_for_cleanup(&session);
                let mut network_client = lock_network_client(network_client_mutex);
                network_client.release_connection_after_session_close(&server_addr);
                return Err(map_metadata_error(error));
            }
        };
    {
        let network_client = lock_network_client(network_client_mutex);
        if let Err(error) = uniqueness::ensure_unique_network_backend(
            &network_client,
            &server_addr,
            &metadata.backend_id,
            None,
        ) {
            drop(network_client);
            let _ = cleanup::close_session_for_cleanup(&session);
            return Err(error);
        }
    }

    let snapshot = {
        let mut network_client = lock_network_client(network_client_mutex);
        let draft = network_client
            .draft_mut(&request.draft_id)
            .ok_or_else(|| validation::draft_not_found_error(&request.draft_id))?;
        draft.insert_item(NetworkDraftItem {
            key,
            disk_name,
            auth_material: request.claim_code,
            session,
            metadata,
        });
        map_draft_snapshot(draft)
    };

    Ok(snapshot)
}

pub fn remove_network_draft_item(
    network_client_mutex: &Mutex<NetworkClientState>,
    request: RemoveNetworkDraftItemRequest,
) -> Result<NetworkCreateDraftSnapshot, ApiError> {
    let removed_item = {
        let mut network_client = lock_network_client(network_client_mutex);
        let draft = network_client
            .draft_mut(&request.draft_id)
            .ok_or_else(|| validation::draft_not_found_error(&request.draft_id))?;
        draft
            .remove_item(&request.remote_disk_id)
            .ok_or_else(|| validation::draft_item_not_found_error(&request.remote_disk_id))?
    };
    let _ = cleanup::close_session_for_cleanup(&removed_item.session);

    let network_client = lock_network_client(network_client_mutex);
    let draft = network_client
        .draft(&request.draft_id)
        .ok_or_else(|| validation::draft_not_found_error(&request.draft_id))?;
    Ok(map_draft_snapshot(draft))
}

pub fn submit_network_draft(
    backend: &BackendContext,
    runtime_store: &mut DiskRuntimeStore,
    network_client_mutex: &Mutex<NetworkClientState>,
    request: SubmitNetworkDraftRequest,
) -> Result<(), ApiError> {
    let draft = {
        let network_client = lock_network_client(network_client_mutex);
        let draft = network_client
            .draft(&request.draft_id)
            .ok_or_else(|| validation::draft_not_found_error(&request.draft_id))?;
        if draft.items.is_empty() {
            return Err(ApiError::new(
                "network-draft-empty",
                "当前没有可提交的网络盘",
                Some(request.draft_id.clone()),
            ));
        }
        for item in draft.items.values() {
            uniqueness::ensure_unique_network_key(
                runtime_store,
                &network_client,
                &item.key,
                Some(request.draft_id.as_str()),
            )?;
            uniqueness::ensure_unique_network_backend(
                &network_client,
                &draft.server_addr,
                &item.metadata.backend_id,
                Some(request.draft_id.as_str()),
            )?;
        }
        draft.clone()
    };

    let mut inserted_local_disk_ids = Vec::new();
    for item in draft.items.values() {
        let local_disk_id = runtime_store.allocate_local_disk_id();
        runtime_store.insert_runtime(DiskRuntime::new_network(
            local_disk_id.clone(),
            item.disk_name.clone(),
            false,
            draft.server_addr.clone(),
            item.key.remote_disk_id.clone(),
            item.auth_material.clone(),
            item.metadata.disk_size_bytes,
            false,
            item.metadata.read_only,
        ));
        inserted_local_disk_ids.push(local_disk_id);
    }

    if let Err(error) = persistence_service::save_client_state(backend, runtime_store) {
        for local_disk_id in inserted_local_disk_ids.iter().rev() {
            let _ = runtime_store.remove_runtime(local_disk_id);
        }
        return Err(error);
    }

    {
        let mut network_client = lock_network_client(network_client_mutex);
        let draft = network_client
            .remove_draft(&request.draft_id)
            .ok_or_else(|| validation::draft_not_found_error(&request.draft_id))?;
        for item in draft.items.into_values() {
            network_client.insert_opened_session(OpenedNetworkDiskSession {
                key: item.key,
                session: item.session,
                metadata: item.metadata,
            });
        }
    }

    Ok(())
}

pub fn dispose_network_draft(
    network_client_mutex: &Mutex<NetworkClientState>,
    request: DisposeNetworkDraftRequest,
) -> Result<(), ApiError> {
    let draft = {
        let mut network_client = lock_network_client(network_client_mutex);
        network_client
            .remove_draft(&request.draft_id)
            .ok_or_else(|| validation::draft_not_found_error(&request.draft_id))?
    };

    for item in draft.items.into_values() {
        let _ = cleanup::close_session_for_cleanup(&item.session);
    }

    let mut network_client = lock_network_client(network_client_mutex);
    network_client.cleanup_connection_if_idle(&draft.server_addr);
    Ok(())
}

fn map_draft_snapshot(draft: &NetworkCreateDraft) -> NetworkCreateDraftSnapshot {
    NetworkCreateDraftSnapshot {
        draft_id: draft.draft_id.clone(),
        server_addr: draft.server_addr.clone(),
        items: draft
            .items
            .values()
            .map(|item| NetworkDraftItemSnapshot {
                disk_name: item.disk_name.clone(),
                server_addr: item.key.server_addr.clone(),
                remote_disk_id: item.key.remote_disk_id.clone(),
                capacity_bytes: item.metadata.disk_size_bytes,
                read_only: item.metadata.read_only,
            })
            .collect(),
    }
}

fn map_connect_error(error: NetworkClientError) -> ApiError {
    ApiError::new(
        "network-connect-failed",
        "测试连接失败",
        Some(error.to_string()),
    )
}

fn map_auth_error(error: NetworkClientError) -> ApiError {
    ApiError::new("network-auth-failed", "认证失败", Some(error.to_string()))
}

fn map_open_error(error: NetworkClientError) -> ApiError {
    ApiError::new(
        "network-session-open-failed",
        "会话打开失败",
        Some(error.to_string()),
    )
}

fn map_metadata_error(error: NetworkClientError) -> ApiError {
    ApiError::new(
        "network-metadata-failed",
        "元数据获取失败",
        Some(error.to_string()),
    )
}

#[allow(dead_code)]
fn _assert_session_metadata_is_copy(_: SessionMetadata, _: DiskSession) {}

#[cfg(test)]
mod tests {
    use std::env;
    use std::ffi::OsString;
    use std::fs;
    use std::net::TcpListener;
    use std::path::Path;
    use std::path::PathBuf;
    use std::sync::Arc;
    use std::sync::Mutex;
    use std::sync::OnceLock;
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
    use network_core::protocol::SESSION_CLOSE_REASON_NORMAL_CLOSE;
    use network_core::protocol::SessionCloseNotice;
    use network_core::protocol::parse_header;
    use network_core::protocol::parse_request_header;
    use network_core::test_support::expect_client_hello;
    use network_core::test_support::stage_connection;
    use network_core::transport::MAX_FRAME_PAYLOAD_BYTES;
    use network_core::transport::TransportEndpoint;
    use network_core::transport::read_frame_into;
    use network_core::transport::write_frame;

    use super::AddNetworkDraftItemRequest;
    use super::CreateNetworkDraftRequest;
    use super::DisposeNetworkDraftRequest;
    use super::SubmitNetworkDraftRequest;
    use super::add_network_draft_item;
    use super::create_network_draft;
    use super::dispose_network_draft;
    use crate::state::client_config;
    use crate::state::disk_runtime::DiskRuntime;
    use crate::state::disk_runtime::DiskRuntimeStatus;
    use crate::state::disk_runtime::DiskRuntimeStore;
    use crate::state::network_client::NetworkClientState;
    use crate::state::network_client::NetworkCreateDraft;
    use crate::state::network_client::NetworkDiskKey;
    use crate::state::network_client::NetworkDraftItem;
    use crate::workflow::network_draft as network_draft_workflow;

    static TEST_HOME_LOCK: OnceLock<Mutex<()>> = OnceLock::new();

    struct TestHomeGuard {
        previous_userprofile: Option<OsString>,
        previous_home: Option<OsString>,
        path: PathBuf,
    }

    struct ConnectedSessionHarness {
        server_addr: String,
        connection: Arc<GatewayConnection>,
        stop: Arc<AtomicBool>,
        server: thread::JoinHandle<()>,
    }

    impl TestHomeGuard {
        fn new(path: PathBuf) -> Self {
            fs::create_dir_all(&path).expect("test home directory should be created");
            let previous_userprofile = env::var_os("USERPROFILE");
            let previous_home = env::var_os("HOME");
            unsafe {
                env::set_var("USERPROFILE", &path);
                env::set_var("HOME", &path);
            }
            Self {
                previous_userprofile,
                previous_home,
                path,
            }
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

    impl Drop for TestHomeGuard {
        fn drop(&mut self) {
            match &self.previous_userprofile {
                Some(value) => unsafe { env::set_var("USERPROFILE", value) },
                None => unsafe { env::remove_var("USERPROFILE") },
            }
            match &self.previous_home {
                Some(value) => unsafe { env::set_var("HOME", value) },
                None => unsafe { env::remove_var("HOME") },
            }
            let _ = fs::remove_dir_all(&self.path);
        }
    }

    fn with_test_home<T>(name: &str, run: impl FnOnce(&Path) -> T) -> T {
        let _guard = TEST_HOME_LOCK
            .get_or_init(|| Mutex::new(()))
            .lock()
            .expect("test home lock should not be poisoned");
        let path = env::temp_dir().join(format!(
            "tauri-client-network-draft-flow-{}-{}",
            std::process::id(),
            name
        ));
        let _ = fs::remove_dir_all(&path);
        let home_guard = TestHomeGuard::new(path);
        run(&home_guard.path)
    }

    fn sample_metadata() -> SessionMetadata {
        SessionMetadata {
            disk_size_bytes: 4096,
            max_io_bytes: network_core::protocol::MAX_DATA_PLANE_RAW_BYTES,
            read_only: false,
            backend_id: [0; 16],
        }
    }

    fn sample_metadata_with_backend(backend_id: [u8; 16]) -> SessionMetadata {
        SessionMetadata {
            backend_id,
            ..sample_metadata()
        }
    }

    fn build_draft_with_item(
        draft_id: &str,
        server_addr: &str,
        remote_disk_id: &str,
        session_id: u64,
    ) -> (NetworkCreateDraft, DiskSession) {
        let connection = stage_connection(TransportEndpoint::new(server_addr), session_id);
        let session =
            DiskSession::new(Arc::clone(&connection), session_id).expect("session should build");
        let session_handle = session.clone();

        let mut draft =
            NetworkCreateDraft::new(draft_id.to_string(), server_addr.to_string(), connection);
        draft.insert_item(NetworkDraftItem {
            key: NetworkDiskKey::new(server_addr, remote_disk_id),
            disk_name: "network-disk".to_string(),
            auth_material: "claim-1".to_string(),
            session,
            metadata: sample_metadata(),
        });

        (draft, session_handle)
    }

    #[test]
    fn submit_network_draft_moves_items_to_opened_sessions_and_persists_runtime() {
        with_test_home("submit-success", |_| {
            let backend = BackendContext::default();
            let mut runtime_store = DiskRuntimeStore::default();
            let harness = ConnectedSessionHarness::new(31);
            let mut draft = NetworkCreateDraft::new(
                "draft-1".to_string(),
                harness.server_addr.clone(),
                Arc::clone(&harness.connection),
            );
            draft.insert_item(NetworkDraftItem {
                key: NetworkDiskKey::new(&harness.server_addr, "A1b2C3d4E5f6G7h8"),
                disk_name: "network-disk".to_string(),
                auth_material: "claim-1".to_string(),
                session: harness.session(31),
                metadata: sample_metadata(),
            });
            let key = NetworkDiskKey::new(&harness.server_addr, "A1b2C3d4E5f6G7h8");

            let mut network_client = NetworkClientState::default();
            network_client.insert_draft(draft);
            let network_client_mutex = Mutex::new(network_client);

            network_draft_workflow::submit_network_draft(
                &backend,
                &mut runtime_store,
                &network_client_mutex,
                SubmitNetworkDraftRequest {
                    draft_id: "draft-1".to_string(),
                },
            )
            .expect("submit should succeed");

            let runtime = runtime_store
                .find_runtime("disk-1")
                .expect("runtime should be inserted");
            assert_eq!(runtime.status(), &DiskRuntimeStatus::Unmounted);
            assert_eq!(runtime.server_addr(), Some(harness.server_addr.as_str()));
            assert_eq!(runtime.remote_disk_id(), Some("A1b2C3d4E5f6G7h8"));
            assert_eq!(runtime.auth_material(), Some("claim-1"));
            assert_eq!(runtime.capacity_bytes(), 4096);

            let network_client = network_client_mutex
                .lock()
                .expect("network client mutex should not be poisoned");
            assert!(network_client.draft("draft-1").is_none());
            assert!(network_client.opened_session(&key).is_some());

            let persisted = client_config::load_client_config()
                .expect("persisted client config should be readable");
            assert_eq!(persisted.disks.len(), 1);
            assert_eq!(persisted.disks[0].local_disk_id, "disk-1");
            assert_eq!(persisted.disks[0].disk_name, "network-disk");

            harness.shutdown();
        });
    }

    #[test]
    fn submit_network_draft_keeps_existing_mounted_runtime_mounted() {
        with_test_home("submit-keeps-mounted", |_| {
            let backend = BackendContext::default();
            let existing_harness = ConnectedSessionHarness::new(33);
            let draft_harness = ConnectedSessionHarness::new(34);
            let existing_key =
                NetworkDiskKey::new(&existing_harness.server_addr, "Z9y8X7w6V5u4T3s2");

            let mut runtime_store = DiskRuntimeStore::default();
            let mut existing_runtime = DiskRuntime::new_network(
                "disk-1".to_string(),
                "mounted-disk".to_string(),
                false,
                existing_harness.server_addr.clone(),
                "Z9y8X7w6V5u4T3s2".to_string(),
                "claim-mounted".to_string(),
                1,
                false,
                true,
            );
            existing_runtime.set_mounted(7);
            runtime_store.insert_runtime(existing_runtime);

            let mut draft = NetworkCreateDraft::new(
                "draft-1".to_string(),
                draft_harness.server_addr.clone(),
                Arc::clone(&draft_harness.connection),
            );
            draft.insert_item(NetworkDraftItem {
                key: NetworkDiskKey::new(&draft_harness.server_addr, "A1b2C3d4E5f6G7h8"),
                disk_name: "new-network-disk".to_string(),
                auth_material: "claim-draft".to_string(),
                session: draft_harness.session(34),
                metadata: sample_metadata(),
            });

            let mut network_client = NetworkClientState::default();
            network_client.insert_opened_session(
                crate::state::network_client::OpenedNetworkDiskSession {
                    key: existing_key.clone(),
                    session: existing_harness.session(33),
                    metadata: sample_metadata(),
                },
            );
            network_client.insert_draft(draft);
            let network_client_mutex = Mutex::new(network_client);

            network_draft_workflow::submit_network_draft(
                &backend,
                &mut runtime_store,
                &network_client_mutex,
                SubmitNetworkDraftRequest {
                    draft_id: "draft-1".to_string(),
                },
            )
            .expect("submit should succeed");

            let existing_runtime = runtime_store
                .find_runtime("disk-1")
                .expect("mounted runtime should exist");
            assert_eq!(
                existing_runtime.status(),
                &DiskRuntimeStatus::Mounted { target_id: 7 }
            );
            assert_eq!(existing_runtime.capacity_bytes(), 4096);
            assert!(!existing_runtime.source_read_only());
            assert!(!existing_runtime.configured_read_only());

            let new_runtime = runtime_store
                .find_runtime("disk-2")
                .expect("new runtime should be inserted");
            assert_eq!(new_runtime.status(), &DiskRuntimeStatus::Unmounted);

            existing_harness.shutdown();
            draft_harness.shutdown();
        });
    }

    #[test]
    fn submit_network_draft_rejects_duplicate_runtime_and_keeps_draft() {
        let backend = BackendContext::default();
        let mut runtime_store = DiskRuntimeStore::default();
        runtime_store.insert_runtime(DiskRuntime::new_network(
            "disk-1".to_string(),
            "existing-disk".to_string(),
            false,
            "127.0.0.1:9011".to_string(),
            "Z9y8X7w6V5u4T3s2".to_string(),
            "claim-existing".to_string(),
            4096,
            false,
            false,
        ));

        let (draft, _) = build_draft_with_item("draft-1", "127.0.0.1:9011", "Z9y8X7w6V5u4T3s2", 41);
        let mut network_client = NetworkClientState::default();
        network_client.insert_draft(draft);
        let network_client_mutex = Mutex::new(network_client);

        let error = network_draft_workflow::submit_network_draft(
            &backend,
            &mut runtime_store,
            &network_client_mutex,
            SubmitNetworkDraftRequest {
                draft_id: "draft-1".to_string(),
            },
        )
        .expect_err("duplicate runtime should reject submit");

        assert_eq!(error.code, "network-disk-duplicate");
        assert_eq!(runtime_store.snapshots().len(), 1);

        let network_client = network_client_mutex
            .lock()
            .expect("network client mutex should not be poisoned");
        assert!(network_client.draft("draft-1").is_some());
        assert!(
            network_client
                .opened_session(&NetworkDiskKey::new("127.0.0.1:9011", "Z9y8X7w6V5u4T3s2"))
                .is_none()
        );
    }

    #[test]
    fn add_network_draft_item_rejects_known_backend_conflict_and_closes_session() {
        let listener = TcpListener::bind("127.0.0.1:0").expect("listener should bind");
        let server_addr = listener
            .local_addr()
            .expect("server addr should exist")
            .to_string();
        let server = thread::spawn(move || {
            let (mut stream, _) = listener.accept().expect("accept should succeed");
            expect_client_hello(&mut stream);
            let mut buffer = vec![0u8; MAX_FRAME_PAYLOAD_BYTES];

            let auth_start = read_frame_into(&mut stream, &mut buffer)
                .expect("auth start should be readable")
                .to_vec();
            let auth_start_header =
                parse_request_header(&auth_start).expect("auth start header should parse");
            assert_eq!(auth_start_header.op_code, ClientOperationCode::AuthStart);

            let mut auth_start_body = Vec::new();
            auth_start_body.push(1);
            auth_start_body.extend_from_slice(&30u16.to_be_bytes());
            auth_start_body.extend_from_slice(&[5u8; 16]);
            auth_start_body.extend_from_slice(&3u16.to_be_bytes());
            auth_start_body.extend_from_slice(b"tok");
            let auth_start_response = ProtocolHeader {
                protocol_version: PROTOCOL_VERSION,
                header_len: HEADER_SIZE as u8,
                op_code: ClientOperationCode::AuthStart,
                flags: FLAG_RESPONSE,
                status_code: ProtocolStatusCode::Ok,
                reserved: 0,
                request_id: auth_start_header.request_id,
                session_id: 0,
            }
            .encode(&auth_start_body);
            write_frame(&mut stream, &auth_start_response)
                .expect("auth start response should be writable");

            let auth_finish = read_frame_into(&mut stream, &mut buffer)
                .expect("auth finish should be readable")
                .to_vec();
            let auth_finish_header =
                parse_request_header(&auth_finish).expect("auth finish header should parse");
            assert_eq!(auth_finish_header.op_code, ClientOperationCode::AuthFinish);

            let auth_finish_response = ProtocolHeader {
                protocol_version: PROTOCOL_VERSION,
                header_len: HEADER_SIZE as u8,
                op_code: ClientOperationCode::AuthFinish,
                flags: FLAG_RESPONSE,
                status_code: ProtocolStatusCode::Ok,
                reserved: 0,
                request_id: auth_finish_header.request_id,
                session_id: 0,
            }
            .encode(&88u64.to_be_bytes());
            write_frame(&mut stream, &auth_finish_response)
                .expect("auth finish response should be writable");

            let session_open = read_frame_into(&mut stream, &mut buffer)
                .expect("session open should be readable")
                .to_vec();
            let session_open_header =
                parse_request_header(&session_open).expect("session open header should parse");
            assert_eq!(
                session_open_header.op_code,
                ClientOperationCode::SessionOpen
            );

            let session_open_response = ProtocolHeader {
                protocol_version: PROTOCOL_VERSION,
                header_len: HEADER_SIZE as u8,
                op_code: ClientOperationCode::SessionOpen,
                flags: FLAG_RESPONSE,
                status_code: ProtocolStatusCode::Ok,
                reserved: 0,
                request_id: session_open_header.request_id,
                session_id: 77,
            }
            .encode(&[]);
            write_frame(&mut stream, &session_open_response)
                .expect("session open response should be writable");

            let session_describe = read_frame_into(&mut stream, &mut buffer)
                .expect("session describe should be readable")
                .to_vec();
            let session_describe_header =
                parse_request_header(&session_describe).expect("describe header should parse");
            assert_eq!(
                session_describe_header.op_code,
                ClientOperationCode::SessionDescribe
            );
            assert_eq!(session_describe_header.session_id, 77);

            let mut describe_body = Vec::new();
            describe_body.extend_from_slice(&4096u64.to_be_bytes());
            describe_body
                .extend_from_slice(&network_core::protocol::MAX_DATA_PLANE_RAW_BYTES.to_be_bytes());
            describe_body.extend_from_slice(&0u16.to_be_bytes());
            describe_body.extend_from_slice(&0u16.to_be_bytes());
            describe_body.extend_from_slice(&[9u8; 16]);
            let session_describe_response = ProtocolHeader {
                protocol_version: PROTOCOL_VERSION,
                header_len: HEADER_SIZE as u8,
                op_code: ClientOperationCode::SessionDescribe,
                flags: FLAG_RESPONSE,
                status_code: ProtocolStatusCode::Ok,
                reserved: 0,
                request_id: session_describe_header.request_id,
                session_id: 77,
            }
            .encode(&describe_body);
            write_frame(&mut stream, &session_describe_response)
                .expect("describe response should be writable");

            let session_close = read_frame_into(&mut stream, &mut buffer)
                .expect("session close notice should be readable")
                .to_vec();
            let session_close_header =
                parse_header(&session_close).expect("session close header should parse");
            assert_eq!(session_close_header.flags, FLAG_NOTICE);
            assert_eq!(
                session_close_header.op_code,
                ClientOperationCode::SessionCloseNotice
            );
            let close_notice =
                SessionCloseNotice::decode_notice(&session_close).expect("close notice decode");
            assert_eq!(close_notice.session_id, 77);
            assert_eq!(close_notice.reason_code, SESSION_CLOSE_REASON_NORMAL_CLOSE);
        });

        let runtime_store = DiskRuntimeStore::default();
        let mut network_client = NetworkClientState::default();
        let existing_connection = stage_connection(TransportEndpoint::new(&server_addr), 13);
        let existing_session =
            DiskSession::new(existing_connection, 13).expect("existing session should build");
        network_client.insert_opened_session(
            crate::state::network_client::OpenedNetworkDiskSession {
                key: NetworkDiskKey::new(&server_addr, "existing-ro"),
                session: existing_session,
                metadata: sample_metadata_with_backend([9; 16]),
            },
        );
        let network_client_mutex = Mutex::new(network_client);

        let draft = create_network_draft(
            &network_client_mutex,
            CreateNetworkDraftRequest {
                server_addr: server_addr.clone(),
            },
        )
        .expect("draft creation should succeed");
        let error = add_network_draft_item(
            &runtime_store,
            &network_client_mutex,
            AddNetworkDraftItemRequest {
                draft_id: draft.draft_id.clone(),
                disk_name: "network-disk".to_string(),
                claim_code: "A1b2C3d4E5f6G7h8abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789ab".to_string(),
            },
        )
        .expect_err("backend conflict should reject add");

        assert_eq!(error.code, "network-backend-conflict");
        let network_client = network_client_mutex
            .lock()
            .expect("network client mutex should not be poisoned");
        let draft = network_client
            .draft(&draft.draft_id)
            .expect("draft should remain after rejection");
        assert!(draft.items.is_empty());
        assert!(draft.connection.is_connected());
        assert!(
            network_client
                .opened_session(&NetworkDiskKey::new(&server_addr, "existing-ro"))
                .is_some()
        );

        server.join().expect("server should join");
    }

    #[test]
    fn submit_network_draft_rejects_backend_conflict_and_keeps_draft() {
        let backend = BackendContext::default();
        let mut runtime_store = DiskRuntimeStore::default();
        let harness = ConnectedSessionHarness::new(61);
        let live_connection = stage_connection(TransportEndpoint::new(&harness.server_addr), 62);
        let live_session =
            DiskSession::new(live_connection, 62).expect("live session should build");

        let mut draft = NetworkCreateDraft::new(
            "draft-1".to_string(),
            harness.server_addr.clone(),
            Arc::clone(&harness.connection),
        );
        draft.insert_item(NetworkDraftItem {
            key: NetworkDiskKey::new(&harness.server_addr, "draft-ro"),
            disk_name: "draft-disk".to_string(),
            auth_material: "claim-draft".to_string(),
            session: harness.session(61),
            metadata: sample_metadata_with_backend([4; 16]),
        });

        let mut network_client = NetworkClientState::default();
        network_client.insert_opened_session(
            crate::state::network_client::OpenedNetworkDiskSession {
                key: NetworkDiskKey::new(&harness.server_addr, "live-ro"),
                session: live_session,
                metadata: sample_metadata_with_backend([4; 16]),
            },
        );
        network_client.insert_draft(draft);
        let network_client_mutex = Mutex::new(network_client);

        let error = network_draft_workflow::submit_network_draft(
            &backend,
            &mut runtime_store,
            &network_client_mutex,
            SubmitNetworkDraftRequest {
                draft_id: "draft-1".to_string(),
            },
        )
        .expect_err("backend conflict should reject submit");

        assert_eq!(error.code, "network-backend-conflict");
        assert!(runtime_store.snapshots().is_empty());

        let network_client = network_client_mutex
            .lock()
            .expect("network client mutex should not be poisoned");
        assert!(network_client.draft("draft-1").is_some());
        assert!(
            network_client
                .opened_session(&NetworkDiskKey::new(&harness.server_addr, "live-ro"))
                .is_some()
        );

        harness.shutdown();
    }

    #[test]
    fn dispose_network_draft_removes_draft_and_closes_item_sessions() {
        let (draft, session_handle) =
            build_draft_with_item("draft-1", "127.0.0.1:9012", "Q1w2E3r4T5y6U7i8", 51);
        let mut network_client = NetworkClientState::default();
        network_client.insert_draft(draft);
        let network_client_mutex = Mutex::new(network_client);

        dispose_network_draft(
            &network_client_mutex,
            DisposeNetworkDraftRequest {
                draft_id: "draft-1".to_string(),
            },
        )
        .expect("dispose should succeed");

        assert!(session_handle.ensure_usable().is_err());

        let network_client = network_client_mutex
            .lock()
            .expect("network client mutex should not be poisoned");
        assert!(network_client.draft("draft-1").is_none());
    }
}
