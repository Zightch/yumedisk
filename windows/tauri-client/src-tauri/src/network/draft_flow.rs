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
    let connection =
        gateway_ops::acquire_connection(network_client_mutex, &server_addr).map_err(map_connect_error)?;
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

    let auth =
        gateway_ops::authenticate(Arc::clone(&connection), &request.claim_code).map_err(map_auth_error)?;
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
    let metadata = match gateway_ops::describe_session(Arc::clone(&connection), session.session_id()) {
        Ok(metadata) => metadata,
        Err(error) => {
            let _ = cleanup::close_session_for_cleanup(&session);
            let mut network_client = lock_network_client(network_client_mutex);
            network_client.release_connection_after_session_close(&server_addr);
            return Err(map_metadata_error(error));
        }
    };

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
    use std::sync::atomic::AtomicBool;
    use std::sync::atomic::Ordering;
    use std::sync::Mutex;
    use std::sync::OnceLock;
    use std::thread;
    use std::time::Duration;

    use backend_rust::BackendContext;
    use network_core::client::DiskSession;
    use network_core::client::GatewayConnection;
    use network_core::client::SessionMetadata;
    use network_core::test_support::stage_connection;
    use network_core::test_support::expect_client_hello;
    use network_core::transport::TransportEndpoint;

    use super::dispose_network_draft;
    use super::DisposeNetworkDraftRequest;
    use super::SubmitNetworkDraftRequest;
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
            let server_addr = listener.local_addr().expect("local addr should exist").to_string();
            let stop = Arc::new(AtomicBool::new(false));
            let stop_for_server = Arc::clone(&stop);
            let server = thread::spawn(move || {
                let (mut stream, _) = listener.accept().expect("accept should succeed");
                expect_client_hello(&mut stream);
                while !stop_for_server.load(Ordering::SeqCst) {
                    thread::sleep(Duration::from_millis(10));
                }
            });

            let connection = stage_connection(TransportEndpoint::new(server_addr.clone()), session_id);
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
            max_io_bytes: 4096,
            read_only: false,
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
