use std::collections::BTreeMap;
use std::sync::Arc;
use std::sync::Mutex;

use backend_rust::BackendContext;
use backend_rust::DiskConfig;
use backend_rust::Media;
use network_core::client::NetworkClientError;

use crate::api_error::ApiError;
use crate::backend::network_media::NetworkMedia;
use crate::state::disk_runtime::DiskRuntimeStore;
use crate::state::disk_runtime::RemovedDiskRuntime;
use crate::state::network_client::NetworkClientState;
use crate::state::network_client::NetworkDiskKey;
use crate::state::network_client::OpenedNetworkDiskSession;

use super::NETWORK_AUTH_FAILED_REASON;
use super::NETWORK_AUTH_MISMATCH_REASON;
use super::NETWORK_CONNECTION_UNAVAILABLE_REASON;
use super::NETWORK_METADATA_FAILED_REASON;
use super::NETWORK_OPEN_FAILED_REASON;
use super::NETWORK_SESSION_MISSING_REASON;
use super::cleanup;
use super::gateway_ops;
use super::lock_network_client;
use super::validation;

#[derive(Debug, Clone)]
struct NetworkRescanTask {
    local_disk_id: String,
    server_addr: String,
    remote_disk_id: String,
    auth_material: String,
}

pub fn mount_network_disk(
    backend: &BackendContext,
    runtime_store: &mut DiskRuntimeStore,
    network_client_mutex: &Mutex<NetworkClientState>,
    local_disk_id: &str,
) -> Result<u32, ApiError> {
    let key = {
        let runtime = runtime_store
            .find_runtime(local_disk_id)
            .ok_or_else(|| validation::disk_not_found_error(local_disk_id))?;
        if let Some(target_id) = runtime.mounted_target_id() {
            return Err(ApiError::new(
                "disk-already-mounted",
                "磁盘已经处于挂载状态",
                Some(target_id.to_string()),
            ));
        }
        if let Some(reason) = runtime.invalid_reason() {
            return Err(ApiError::new(
                "disk-invalid",
                "磁盘当前无效，不能挂载",
                Some(reason.to_string()),
            ));
        }
        let (server_addr, remote_disk_id) = runtime
            .network_key()
            .ok_or_else(|| ApiError::new("disk-not-network", "磁盘不是网络盘", None))?;
        NetworkDiskKey::new(server_addr, remote_disk_id)
    };

    let opened_session = {
        let network_client = lock_network_client(network_client_mutex);
        network_client.opened_session(&key)
    };
    let Some(opened_session) = opened_session else {
        if let Some(runtime) = runtime_store.find_runtime_mut(local_disk_id) {
            runtime.set_network_invalid(NETWORK_SESSION_MISSING_REASON.to_string());
        }
        return Err(ApiError::new(
            "disk-invalid",
            "磁盘当前无效，不能挂载",
            Some(NETWORK_SESSION_MISSING_REASON.to_string()),
        ));
    };

    if opened_session.session.ensure_usable().is_err() {
        {
            let mut network_client = lock_network_client(network_client_mutex);
            let _ = network_client.remove_opened_session(&key);
            network_client.release_connection_after_session_close(&key.server_addr);
        }
        if let Some(runtime) = runtime_store.find_runtime_mut(local_disk_id) {
            runtime.set_network_invalid(NETWORK_SESSION_MISSING_REASON.to_string());
        }
        return Err(ApiError::new(
            "disk-invalid",
            "磁盘当前无效，不能挂载",
            Some(NETWORK_SESSION_MISSING_REASON.to_string()),
        ));
    }

    let media = NetworkMedia::bind(
        key.remote_disk_id.clone(),
        opened_session.session.clone(),
        opened_session.metadata,
    )
    .map(|media| {
        media.with_invalidation_handler({
            let network_client = lock_network_client(network_client_mutex);
            network_client.media_invalidation_handler(local_disk_id.to_string())
        })
    })
    .map_err(map_mount_error)?;
    let disk_config = DiskConfig {
        disk_size_bytes: opened_session.metadata.disk_size_bytes,
        read_only: opened_session.metadata.read_only,
        ..DiskConfig::default()
    };
    let boxed_media: Box<dyn Media> = Box::new(media);
    let mut error_text = String::new();
    let target_id =
        match backend.try_create_managed_disk(disk_config, boxed_media, Some(&mut error_text)) {
            Ok(target_id) => target_id,
            Err(_) => {
                return Err(ApiError::new(
                    "mount-disk-failed",
                    "挂载磁盘失败",
                    Some(error_text),
                ));
            }
        };

    let runtime = runtime_store
        .find_runtime_mut(local_disk_id)
        .ok_or_else(|| validation::disk_not_found_error(local_disk_id))?;
    runtime.set_mounted(target_id);
    Ok(target_id)
}

pub fn eject_network_disk(
    backend: &BackendContext,
    runtime_store: &mut DiskRuntimeStore,
    local_disk_id: &str,
) -> Result<(), ApiError> {
    let runtime = runtime_store
        .find_runtime_mut(local_disk_id)
        .ok_or_else(|| validation::disk_not_found_error(local_disk_id))?;
    let Some(target_id) = runtime.mounted_target_id() else {
        return Err(ApiError::new(
            "disk-not-mounted",
            "磁盘当前未挂载，不能拔出",
            Some(local_disk_id.to_string()),
        ));
    };

    let mut error_text = String::new();
    let media = backend
        .remove_managed_disk_with_media(target_id, Some(&mut error_text))
        .ok_or_else(|| ApiError::new("eject-disk-failed", "拔出磁盘失败", Some(error_text)))?;
    drop(media);

    runtime.set_network_unmounted(runtime.capacity_bytes(), runtime.read_only());
    Ok(())
}

pub fn prepare_deleted_network_runtime(
    backend: &BackendContext,
    removed_runtime: &mut RemovedDiskRuntime,
    network_client_mutex: &Mutex<NetworkClientState>,
) -> Result<(), ApiError> {
    if let Some(target_id) = removed_runtime.runtime.mounted_target_id() {
        let mut error_text = String::new();
        let media = backend
            .remove_managed_disk_with_media(target_id, Some(&mut error_text))
            .ok_or_else(|| ApiError::new("delete-disk-failed", "删除磁盘失败", Some(error_text)))?;
        drop(media);
        removed_runtime.runtime.set_network_unmounted(
            removed_runtime.runtime.capacity_bytes(),
            removed_runtime.runtime.read_only(),
        );
    }

    if let Some((server_addr, remote_disk_id)) = removed_runtime.runtime.network_key() {
        let key = NetworkDiskKey::new(server_addr.clone(), remote_disk_id);
        let opened_session = {
            let mut network_client = lock_network_client(network_client_mutex);
            let opened_session = network_client.remove_opened_session(&key);
            network_client.release_connection_after_session_close(&server_addr);
            opened_session
        };
        if let Some(opened_session) = opened_session {
            let _ = cleanup::close_session_for_cleanup(&opened_session.session);
        }
    }

    removed_runtime
        .runtime
        .set_network_invalid(NETWORK_SESSION_MISSING_REASON.to_string());
    Ok(())
}

pub fn rescan_network_runtimes(
    runtime_store: &mut DiskRuntimeStore,
    network_client_mutex: &Mutex<NetworkClientState>,
) {
    let tasks = runtime_store
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
            })
        })
        .collect::<Vec<_>>();

    let mut tasks_by_server = BTreeMap::<String, Vec<NetworkRescanTask>>::new();
    for task in tasks {
        tasks_by_server
            .entry(task.server_addr.clone())
            .or_default()
            .push(task);
    }

    for (server_addr, tasks) in tasks_by_server {
        for task in tasks {
            let key = NetworkDiskKey::new(task.server_addr.clone(), task.remote_disk_id.clone());
            if let Some(opened_session) = {
                let network_client = lock_network_client(network_client_mutex);
                network_client.opened_session(&key)
            } {
                if opened_session.session.ensure_usable().is_ok() {
                    cleanup::set_network_runtime_unmounted(
                        runtime_store,
                        &task.local_disk_id,
                        opened_session.metadata,
                    );
                    continue;
                }

                let _ = cleanup::close_session_for_cleanup(&opened_session.session);
                let mut network_client = lock_network_client(network_client_mutex);
                let _ = network_client.remove_opened_session(&key);
                network_client.release_connection_after_session_close(&server_addr);
            }

            let connection = match gateway_ops::acquire_connection(network_client_mutex, &server_addr) {
                Ok(connection) => connection,
                Err(_) => {
                    cleanup::set_network_runtime_invalid(
                        runtime_store,
                        &task.local_disk_id,
                        NETWORK_CONNECTION_UNAVAILABLE_REASON,
                    );
                    continue;
                }
            };

            let auth = match gateway_ops::authenticate(Arc::clone(&connection), &task.auth_material) {
                Ok(auth) => auth,
                Err(_) => {
                    cleanup::set_network_runtime_invalid(
                        runtime_store,
                        &task.local_disk_id,
                        NETWORK_AUTH_FAILED_REASON,
                    );
                    continue;
                }
            };

            if auth.disk_id() != task.remote_disk_id {
                let _ = connection.discard_auth_grant(auth.auth_id());
                cleanup::set_network_runtime_invalid(
                    runtime_store,
                    &task.local_disk_id,
                    NETWORK_AUTH_MISMATCH_REASON,
                );
                continue;
            }

            let session = match gateway_ops::open_session(Arc::clone(&connection), &auth) {
                Ok(session) => session,
                Err(_) => {
                    cleanup::set_network_runtime_invalid(
                        runtime_store,
                        &task.local_disk_id,
                        NETWORK_OPEN_FAILED_REASON,
                    );
                    continue;
                }
            };

            let metadata =
                match gateway_ops::describe_session(Arc::clone(&connection), session.session_id()) {
                    Ok(metadata) => metadata,
                    Err(_) => {
                        let _ = cleanup::close_session_for_cleanup(&session);
                        let mut network_client = lock_network_client(network_client_mutex);
                        network_client.release_connection_after_session_close(&server_addr);
                        cleanup::set_network_runtime_invalid(
                            runtime_store,
                            &task.local_disk_id,
                            NETWORK_METADATA_FAILED_REASON,
                        );
                        continue;
                    }
                };

            {
                let mut network_client = lock_network_client(network_client_mutex);
                network_client.insert_opened_session(OpenedNetworkDiskSession {
                    key,
                    session,
                    metadata,
                });
            }
            cleanup::set_network_runtime_unmounted(runtime_store, &task.local_disk_id, metadata);
        }
    }
}

fn map_mount_error(error: NetworkClientError) -> ApiError {
    ApiError::new("mount-disk-failed", "挂载磁盘失败", Some(error.to_string()))
}

#[cfg(test)]
mod tests {
    use std::net::TcpListener;
    use std::sync::Arc;
    use std::sync::atomic::AtomicBool;
    use std::sync::atomic::Ordering;
    use std::sync::Mutex;
    use std::thread;
    use std::time::Duration;

    use backend_rust::BackendContext;
    use network_core::client::DiskSession;
    use network_core::client::GatewayConnection;
    use network_core::client::SessionMetadata;
    use network_core::test_support::expect_client_hello;
    use network_core::test_support::stage_connection;
    use network_core::transport::TransportEndpoint;

    use super::mount_network_disk;
    use super::prepare_deleted_network_runtime;
    use super::rescan_network_runtimes;
    use crate::network::NETWORK_SESSION_MISSING_REASON;
    use crate::state::disk_runtime::DiskRuntime;
    use crate::state::disk_runtime::DiskRuntimeStatus;
    use crate::state::disk_runtime::DiskRuntimeStore;
    use crate::state::network_client::NetworkClientState;
    use crate::state::network_client::NetworkDiskKey;
    use crate::state::network_client::OpenedNetworkDiskSession;

    struct ConnectedSessionHarness {
        server_addr: String,
        connection: Arc<GatewayConnection>,
        stop: Arc<AtomicBool>,
        server: thread::JoinHandle<()>,
    }

    fn sample_metadata() -> SessionMetadata {
        SessionMetadata {
            disk_size_bytes: 4096,
            max_io_bytes: 4096,
            read_only: false,
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

    fn build_opened_session(
        server_addr: &str,
        remote_disk_id: &str,
        session_id: u64,
    ) -> (NetworkDiskKey, OpenedNetworkDiskSession, DiskSession) {
        let connection = stage_connection(TransportEndpoint::new(server_addr), session_id);
        let session =
            DiskSession::new(Arc::clone(&connection), session_id).expect("session should build");
        let session_handle = session.clone();
        let key = NetworkDiskKey::new(server_addr, remote_disk_id);
        (
            key.clone(),
            OpenedNetworkDiskSession {
                key,
                session,
                metadata: sample_metadata(),
            },
            session_handle,
        )
    }

    #[test]
    fn mount_network_disk_marks_runtime_invalid_when_session_is_missing() {
        let backend = BackendContext::default();
        let network_client_mutex = Mutex::new(NetworkClientState::default());
        let mut runtime_store = DiskRuntimeStore::default();

        let mut runtime = DiskRuntime::new_network(
            "disk-1".to_string(),
            "network-disk".to_string(),
            false,
            "127.0.0.1:9003".to_string(),
            "Z9y8X7w6V5u4T3s2".to_string(),
            "claim-2".to_string(),
            4096,
            false,
        );
        runtime.set_network_unmounted(4096, false);
        runtime_store.insert_runtime(runtime);

        let error = mount_network_disk(
            &backend,
            &mut runtime_store,
            &network_client_mutex,
            "disk-1",
        )
        .expect_err("mount should fail without live session");

        assert_eq!(error.code, "disk-invalid");
        let runtime = runtime_store
            .find_runtime("disk-1")
            .expect("runtime should exist");
        assert_eq!(
            runtime.status(),
            &DiskRuntimeStatus::Invalid {
                reason: NETWORK_SESSION_MISSING_REASON.to_string(),
            }
        );
    }

    #[test]
    fn mount_network_disk_removes_unusable_opened_session_and_marks_runtime_invalid() {
        let backend = BackendContext::default();
        let (key, opened_session, session_handle) =
            build_opened_session("127.0.0.1:9006", "A1b2C3d4E5f6G7h8", 17);

        let mut network_client = NetworkClientState::default();
        network_client.insert_opened_session(opened_session);
        let network_client_mutex = Mutex::new(network_client);
        session_handle
            .close()
            .expect("session close should succeed before mount");

        let mut runtime_store = DiskRuntimeStore::default();
        let mut runtime = DiskRuntime::new_network(
            "disk-1".to_string(),
            "network-disk".to_string(),
            false,
            "127.0.0.1:9006".to_string(),
            "A1b2C3d4E5f6G7h8".to_string(),
            "claim-1".to_string(),
            4096,
            false,
        );
        runtime.set_network_unmounted(4096, false);
        runtime_store.insert_runtime(runtime);

        let error = mount_network_disk(
            &backend,
            &mut runtime_store,
            &network_client_mutex,
            "disk-1",
        )
        .expect_err("mount should fail when opened session is already unusable");

        assert_eq!(error.code, "disk-invalid");
        let runtime = runtime_store
            .find_runtime("disk-1")
            .expect("runtime should exist");
        assert_eq!(
            runtime.status(),
            &DiskRuntimeStatus::Invalid {
                reason: NETWORK_SESSION_MISSING_REASON.to_string(),
            }
        );

        let network_client = network_client_mutex
            .lock()
            .expect("network client mutex should not be poisoned");
        assert!(network_client.opened_session(&key).is_none());
    }

    #[test]
    fn prepare_deleted_network_runtime_closes_unmounted_opened_session() {
        let backend = BackendContext::default();
        let (key, opened_session, session_handle) =
            build_opened_session("127.0.0.1:9007", "Z9y8X7w6V5u4T3s2", 21);

        let mut network_client = NetworkClientState::default();
        network_client.insert_opened_session(opened_session);
        let network_client_mutex = Mutex::new(network_client);

        let mut runtime_store = DiskRuntimeStore::default();
        let mut runtime = DiskRuntime::new_network(
            "disk-1".to_string(),
            "network-disk".to_string(),
            false,
            "127.0.0.1:9007".to_string(),
            "Z9y8X7w6V5u4T3s2".to_string(),
            "claim-2".to_string(),
            4096,
            false,
        );
        runtime.set_network_unmounted(4096, false);
        runtime_store.insert_runtime(runtime);

        let mut removed_runtime = runtime_store
            .remove_runtime("disk-1")
            .expect("runtime should be removable");
        prepare_deleted_network_runtime(&backend, &mut removed_runtime, &network_client_mutex)
            .expect("unmounted delete should succeed");

        assert!(session_handle.ensure_usable().is_err());
        assert_eq!(
            removed_runtime.runtime.status(),
            &DiskRuntimeStatus::Invalid {
                reason: NETWORK_SESSION_MISSING_REASON.to_string(),
            }
        );

        let network_client = network_client_mutex
            .lock()
            .expect("network client mutex should not be poisoned");
        assert!(network_client.opened_session(&key).is_none());
    }

    #[test]
    fn prepare_deleted_network_runtime_fails_when_mounted_disk_cannot_be_removed() {
        let backend = BackendContext::default();
        let network_client_mutex = Mutex::new(NetworkClientState::default());

        let mut runtime_store = DiskRuntimeStore::default();
        let mut runtime = DiskRuntime::new_network(
            "disk-1".to_string(),
            "network-disk".to_string(),
            false,
            "127.0.0.1:9008".to_string(),
            "M1n2B3v4C5x6Z7a8".to_string(),
            "claim-3".to_string(),
            4096,
            false,
        );
        runtime.set_mounted(7);
        runtime_store.insert_runtime(runtime);

        let mut removed_runtime = runtime_store
            .remove_runtime("disk-1")
            .expect("runtime should be removable");
        let error =
            prepare_deleted_network_runtime(&backend, &mut removed_runtime, &network_client_mutex)
                .expect_err("mounted delete should fail without backend session");

        assert_eq!(error.code, "delete-disk-failed");
        assert_eq!(
            removed_runtime.runtime.status(),
            &DiskRuntimeStatus::Mounted { target_id: 7 }
        );
    }

    #[test]
    fn rescan_network_runtimes_reuses_live_opened_session() {
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
            true,
        ));

        rescan_network_runtimes(&mut runtime_store, &network_client_mutex);

        let runtime = runtime_store
            .find_runtime("disk-1")
            .expect("runtime should exist");
        assert_eq!(runtime.status(), &DiskRuntimeStatus::Unmounted);
        assert_eq!(runtime.capacity_bytes(), 4096);
        assert!(!runtime.read_only());

        let network_client = network_client_mutex
            .lock()
            .expect("network client mutex should not be poisoned");
        assert!(network_client.opened_session(&key).is_some());

        harness.shutdown();
    }
}
