use std::collections::BTreeMap;
use std::sync::Arc;
use std::sync::Mutex;

use backend_rust::BackendContext;
use backend_rust::DiskConfig;
use backend_rust::Media;
use network_core::client::ConnectionAuthenticator;
use network_core::client::DiskSession;
use network_core::client::NetworkClientError;
use network_core::client::SessionDescriber;
use network_core::client::SessionMetadata;
use network_core::client::SessionOpener;

use crate::api_error::ApiError;
use crate::backend::network_media::NetworkMedia;
use crate::backend::persistence_service;
use crate::state::disk_runtime::DiskRuntime;
use crate::state::disk_runtime::DiskRuntimeStore;
use crate::state::disk_runtime::RemovedDiskRuntime;
use crate::state::network_client::NetworkClientEvent;
use crate::state::network_client::NetworkClientState;
use crate::state::network_client::NetworkCreateDraft;
use crate::state::network_client::NetworkDiskKey;
use crate::state::network_client::NetworkDraftItem;
use crate::state::network_client::OpenedNetworkDiskSession;

const NETWORK_SESSION_MISSING_REASON: &str = "网络盘会话未打开";
const NETWORK_CONNECTION_UNAVAILABLE_REASON: &str = "网络盘连接不可用";
const NETWORK_AUTH_FAILED_REASON: &str = "网络盘认证失败";
const NETWORK_OPEN_FAILED_REASON: &str = "网络盘会话打开失败";
const NETWORK_METADATA_FAILED_REASON: &str = "网络盘元数据获取失败";
const NETWORK_AUTH_MISMATCH_REASON: &str = "认证材料与网络盘不匹配";

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

#[derive(Debug, Clone)]
struct NetworkRescanTask {
    local_disk_id: String,
    server_addr: String,
    remote_disk_id: String,
    auth_material: String,
}

#[derive(Debug, Clone)]
struct RemovedDraftSession {
    session: DiskSession,
}

pub fn sync_pending_events(
    backend: &BackendContext,
    runtime_store: &mut DiskRuntimeStore,
    network_client_mutex: &Mutex<NetworkClientState>,
) -> bool {
    let events = {
        let network_client = lock_network_client(network_client_mutex);
        network_client.drain_events()
    };
    let invalidations = {
        let network_client = lock_network_client(network_client_mutex);
        network_client.drain_media_invalidations()
    };

    let mut changed = false;
    for event in events {
        changed |= match event {
            NetworkClientEvent::SessionClosed {
                server_addr,
                session_id,
            } => {
                let opened_session_key = {
                    let network_client = lock_network_client(network_client_mutex);
                    network_client.find_opened_session_by_session_id(&server_addr, session_id)
                };
                if let Some(key) = opened_session_key.as_ref() {
                    invalidate_runtime_by_key(
                        backend,
                        runtime_store,
                        network_client_mutex,
                        key,
                        NETWORK_SESSION_MISSING_REASON,
                    );
                }

                let draft_sessions = {
                    let mut network_client = lock_network_client(network_client_mutex);
                    remove_draft_sessions_by_session_id(
                        &mut network_client,
                        &server_addr,
                        session_id,
                    )
                };
                for draft_session in draft_sessions {
                    let _ = close_session_for_cleanup(&draft_session.session);
                }
                {
                    let mut network_client = lock_network_client(network_client_mutex);
                    network_client.release_connection_after_session_close(&server_addr);
                }
                true
            }
            NetworkClientEvent::ConnectionLost { server_addr } => {
                let opened_session_keys = {
                    let network_client = lock_network_client(network_client_mutex);
                    network_client.opened_session_keys_for_server(&server_addr)
                };

                for key in opened_session_keys {
                    invalidate_runtime_by_key(
                        backend,
                        runtime_store,
                        network_client_mutex,
                        &key,
                        NETWORK_CONNECTION_UNAVAILABLE_REASON,
                    );
                }

                let draft_sessions = {
                    let mut network_client = lock_network_client(network_client_mutex);
                    remove_all_draft_sessions_for_server(&mut network_client, &server_addr)
                };
                for draft_session in draft_sessions {
                    let _ = close_session_for_cleanup(&draft_session.session);
                }
                {
                    let mut network_client = lock_network_client(network_client_mutex);
                    network_client.cleanup_connection_if_idle(&server_addr);
                }
                true
            }
        };
    }

    for local_disk_id in invalidations {
        changed |= invalidate_runtime_by_local_disk_id(
            backend,
            runtime_store,
            network_client_mutex,
            &local_disk_id,
            NETWORK_SESSION_MISSING_REASON,
        );
    }

    changed
}

pub fn test_connection(
    network_client_mutex: &Mutex<NetworkClientState>,
    server_addr: &str,
) -> Result<(), ApiError> {
    let server_addr = validate_server_addr(server_addr)?;
    let mut network_client = lock_network_client(network_client_mutex);
    network_client
        .acquire_connection(server_addr)
        .map(|_| ())
        .map_err(map_connect_error)
}

pub fn create_network_draft(
    network_client_mutex: &Mutex<NetworkClientState>,
    request: CreateNetworkDraftRequest,
) -> Result<NetworkCreateDraftSnapshot, ApiError> {
    let server_addr = validate_server_addr(&request.server_addr)?.to_string();
    let mut network_client = lock_network_client(network_client_mutex);
    let connection = network_client
        .acquire_connection(&server_addr)
        .map_err(map_connect_error)?;
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
    let disk_name = validate_disk_name(&request.disk_name)?.to_string();
    let (server_addr, connection) = {
        let network_client = lock_network_client(network_client_mutex);
        let draft = network_client
            .draft(&request.draft_id)
            .ok_or_else(|| draft_not_found_error(&request.draft_id))?;
        (draft.server_addr.clone(), Arc::clone(&draft.connection))
    };

    let authenticator = ConnectionAuthenticator::new(Arc::clone(&connection));
    let auth = authenticator
        .authenticate(&request.claim_code)
        .map_err(map_auth_error)?;
    let key = NetworkDiskKey::new(server_addr.clone(), auth.disk_id().to_string());

    {
        let network_client = lock_network_client(network_client_mutex);
        if let Err(error) = ensure_unique_network_key(runtime_store, &network_client, &key, None) {
            let _ = connection.discard_auth_grant(auth.auth_id());
            return Err(error);
        }
    }

    let session_id = SessionOpener::new(Arc::clone(&connection))
        .open(&auth)
        .map_err(map_open_error)?;
    let session = DiskSession::new(Arc::clone(&connection), session_id).map_err(map_open_error)?;
    let metadata = match SessionDescriber::new(Arc::clone(&connection)).describe(session_id) {
        Ok(metadata) => metadata,
        Err(error) => {
            let _ = close_session_for_cleanup(&session);
            let mut network_client = lock_network_client(network_client_mutex);
            network_client.release_connection_after_session_close(&server_addr);
            return Err(map_metadata_error(error));
        }
    };

    let snapshot = {
        let mut network_client = lock_network_client(network_client_mutex);
        let draft = network_client
            .draft_mut(&request.draft_id)
            .ok_or_else(|| draft_not_found_error(&request.draft_id))?;
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
            .ok_or_else(|| draft_not_found_error(&request.draft_id))?;
        draft
            .remove_item(&request.remote_disk_id)
            .ok_or_else(|| draft_item_not_found_error(&request.remote_disk_id))?
    };
    let _ = close_session_for_cleanup(&removed_item.session);

    let network_client = lock_network_client(network_client_mutex);
    let draft = network_client
        .draft(&request.draft_id)
        .ok_or_else(|| draft_not_found_error(&request.draft_id))?;
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
            .ok_or_else(|| draft_not_found_error(&request.draft_id))?;
        if draft.items.is_empty() {
            return Err(ApiError::new(
                "network-draft-empty",
                "当前没有可提交的网络盘",
                Some(request.draft_id.clone()),
            ));
        }
        for item in draft.items.values() {
            ensure_unique_network_key(
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
            .ok_or_else(|| draft_not_found_error(&request.draft_id))?;
        for item in draft.items.into_values() {
            network_client.insert_opened_session(OpenedNetworkDiskSession {
                key: item.key,
                session: item.session,
                metadata: item.metadata,
            });
        }
    }

    rescan_network_runtimes(runtime_store, network_client_mutex);
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
            .ok_or_else(|| draft_not_found_error(&request.draft_id))?
    };

    for item in draft.items.into_values() {
        let _ = close_session_for_cleanup(&item.session);
    }

    let mut network_client = lock_network_client(network_client_mutex);
    network_client.cleanup_connection_if_idle(&draft.server_addr);
    Ok(())
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
            .ok_or_else(|| disk_not_found_error(local_disk_id))?;
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
        .ok_or_else(|| disk_not_found_error(local_disk_id))?;
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
        .ok_or_else(|| disk_not_found_error(local_disk_id))?;
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
    runtime_store: &mut RemovedDiskRuntime,
    network_client_mutex: &Mutex<NetworkClientState>,
) -> Result<(), ApiError> {
    if let Some(target_id) = runtime_store.runtime.mounted_target_id() {
        let mut error_text = String::new();
        let media = backend
            .remove_managed_disk_with_media(target_id, Some(&mut error_text))
            .ok_or_else(|| ApiError::new("delete-disk-failed", "删除磁盘失败", Some(error_text)))?;
        drop(media);
        runtime_store.runtime.set_network_unmounted(
            runtime_store.runtime.capacity_bytes(),
            runtime_store.runtime.read_only(),
        );
    }

    if let Some((server_addr, remote_disk_id)) = runtime_store.runtime.network_key() {
        let key = NetworkDiskKey::new(server_addr.clone(), remote_disk_id);
        let opened_session = {
            let mut network_client = lock_network_client(network_client_mutex);
            let opened_session = network_client.remove_opened_session(&key);
            network_client.release_connection_after_session_close(&server_addr);
            opened_session
        };
        if let Some(opened_session) = opened_session {
            let _ = close_session_for_cleanup(&opened_session.session);
        }
    }

    runtime_store
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
                    set_network_runtime_unmounted(
                        runtime_store,
                        &task.local_disk_id,
                        opened_session.metadata,
                    );
                    continue;
                }

                let _ = close_session_for_cleanup(&opened_session.session);
                let mut network_client = lock_network_client(network_client_mutex);
                let _ = network_client.remove_opened_session(&key);
                network_client.release_connection_after_session_close(&server_addr);
            }

            let connection = {
                let mut network_client = lock_network_client(network_client_mutex);
                match network_client.acquire_connection(&server_addr) {
                    Ok(connection) => connection,
                    Err(_) => {
                        set_network_runtime_invalid(
                            runtime_store,
                            &task.local_disk_id,
                            NETWORK_CONNECTION_UNAVAILABLE_REASON,
                        );
                        continue;
                    }
                }
            };

            let authenticator = ConnectionAuthenticator::new(Arc::clone(&connection));
            let auth = match authenticator.authenticate(&task.auth_material) {
                Ok(auth) => auth,
                Err(_) => {
                    set_network_runtime_invalid(
                        runtime_store,
                        &task.local_disk_id,
                        NETWORK_AUTH_FAILED_REASON,
                    );
                    continue;
                }
            };

            if auth.disk_id() != task.remote_disk_id {
                let _ = connection.discard_auth_grant(auth.auth_id());
                set_network_runtime_invalid(
                    runtime_store,
                    &task.local_disk_id,
                    NETWORK_AUTH_MISMATCH_REASON,
                );
                continue;
            }

            let session_id = match SessionOpener::new(Arc::clone(&connection)).open(&auth) {
                Ok(session_id) => session_id,
                Err(_) => {
                    set_network_runtime_invalid(
                        runtime_store,
                        &task.local_disk_id,
                        NETWORK_OPEN_FAILED_REASON,
                    );
                    continue;
                }
            };
            let session = match DiskSession::new(Arc::clone(&connection), session_id) {
                Ok(session) => session,
                Err(_) => {
                    set_network_runtime_invalid(
                        runtime_store,
                        &task.local_disk_id,
                        NETWORK_OPEN_FAILED_REASON,
                    );
                    continue;
                }
            };

            let metadata = match SessionDescriber::new(Arc::clone(&connection)).describe(session_id)
            {
                Ok(metadata) => metadata,
                Err(_) => {
                    let _ = close_session_for_cleanup(&session);
                    let mut network_client = lock_network_client(network_client_mutex);
                    network_client.release_connection_after_session_close(&server_addr);
                    set_network_runtime_invalid(
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
            set_network_runtime_unmounted(runtime_store, &task.local_disk_id, metadata);
        }
    }
}

fn remove_draft_sessions_by_session_id(
    network_client: &mut NetworkClientState,
    server_addr: &str,
    session_id: u64,
) -> Vec<RemovedDraftSession> {
    let draft_ids = network_client.draft_ids_for_server(server_addr);
    let mut removed = Vec::new();

    for draft_id in draft_ids {
        let remote_disk_id = network_client.draft(&draft_id).and_then(|draft| {
            draft.items.iter().find_map(|(remote_disk_id, item)| {
                if item.session.session_id() == session_id {
                    Some(remote_disk_id.clone())
                } else {
                    None
                }
            })
        });
        let Some(remote_disk_id) = remote_disk_id else {
            continue;
        };

        if let Some(draft) = network_client.draft_mut(&draft_id) {
            if let Some(item) = draft.remove_item(&remote_disk_id) {
                removed.push(RemovedDraftSession {
                    session: item.session,
                });
            }
        }
    }

    removed
}

fn remove_all_draft_sessions_for_server(
    network_client: &mut NetworkClientState,
    server_addr: &str,
) -> Vec<RemovedDraftSession> {
    let draft_ids = network_client.draft_ids_for_server(server_addr);
    let mut removed = Vec::new();

    for draft_id in draft_ids {
        let items = if let Some(draft) = network_client.draft_mut(&draft_id) {
            std::mem::take(&mut draft.items)
        } else {
            BTreeMap::new()
        };
        for item in items.into_values() {
            removed.push(RemovedDraftSession {
                session: item.session,
            });
        }
    }

    removed
}

fn ensure_unique_network_key(
    runtime_store: &DiskRuntimeStore,
    network_client: &NetworkClientState,
    key: &NetworkDiskKey,
    excluded_draft_id: Option<&str>,
) -> Result<(), ApiError> {
    if runtime_store.runtimes().any(|runtime| {
        runtime.server_addr() == Some(key.server_addr.as_str())
            && runtime.remote_disk_id() == Some(key.remote_disk_id.as_str())
    }) {
        return Err(ApiError::new(
            "network-disk-duplicate",
            "网络盘已存在",
            Some(format!("{} | {}", key.server_addr, key.remote_disk_id)),
        ));
    }

    for draft_id in network_client.draft_ids_for_server(&key.server_addr) {
        if excluded_draft_id.is_some_and(|excluded| excluded == draft_id) {
            continue;
        }

        let is_duplicate = network_client
            .draft(&draft_id)
            .map(|draft| draft.items.contains_key(&key.remote_disk_id))
            .unwrap_or(false);
        if is_duplicate {
            return Err(ApiError::new(
                "network-disk-duplicate",
                "网络盘已存在",
                Some(format!("{} | {}", key.server_addr, key.remote_disk_id)),
            ));
        }
    }

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

fn invalidate_runtime_by_key(
    backend: &BackendContext,
    runtime_store: &mut DiskRuntimeStore,
    network_client_mutex: &Mutex<NetworkClientState>,
    key: &NetworkDiskKey,
    reason: &str,
) {
    let opened_session = {
        let mut network_client = lock_network_client(network_client_mutex);
        network_client.remove_opened_session(key)
    };
    if let Some(opened_session) = opened_session {
        let _ = close_session_for_cleanup(&opened_session.session);
    }

    for runtime in runtime_store.runtimes_mut() {
        let matches_key = runtime.server_addr() == Some(key.server_addr.as_str())
            && runtime.remote_disk_id() == Some(key.remote_disk_id.as_str());
        if !matches_key {
            continue;
        }

        if let Some(target_id) = runtime.mounted_target_id() {
            let mut error_text = String::new();
            if let Some(media) =
                backend.remove_managed_disk_with_media(target_id, Some(&mut error_text))
            {
                drop(media);
            }
        }
        runtime.set_network_invalid(reason.to_string());
    }
}

fn invalidate_runtime_by_local_disk_id(
    backend: &BackendContext,
    runtime_store: &mut DiskRuntimeStore,
    network_client_mutex: &Mutex<NetworkClientState>,
    local_disk_id: &str,
    reason: &str,
) -> bool {
    let Some((server_addr, remote_disk_id)) = runtime_store
        .find_runtime(local_disk_id)
        .and_then(|runtime| runtime.network_key())
    else {
        return false;
    };

    let key = NetworkDiskKey::new(server_addr, remote_disk_id);
    invalidate_runtime_by_key(backend, runtime_store, network_client_mutex, &key, reason);

    {
        let mut network_client = lock_network_client(network_client_mutex);
        network_client.release_connection_after_session_close(&key.server_addr);
    }

    true
}

fn set_network_runtime_unmounted(
    runtime_store: &mut DiskRuntimeStore,
    local_disk_id: &str,
    metadata: SessionMetadata,
) {
    if let Some(runtime) = runtime_store.find_runtime_mut(local_disk_id) {
        runtime.set_network_unmounted(metadata.disk_size_bytes, metadata.read_only);
    }
}

fn set_network_runtime_invalid(
    runtime_store: &mut DiskRuntimeStore,
    local_disk_id: &str,
    reason: &str,
) {
    if let Some(runtime) = runtime_store.find_runtime_mut(local_disk_id) {
        runtime.set_network_invalid(reason.to_string());
    }
}

fn close_session_for_cleanup(session: &DiskSession) -> bool {
    match session.close() {
        Ok(()) | Err(NetworkClientError::SessionUnavailable) => true,
        Err(_) => false,
    }
}

fn validate_server_addr(server_addr: &str) -> Result<&str, ApiError> {
    let server_addr = server_addr.trim();
    if server_addr.is_empty() {
        return Err(ApiError::new(
            "network-server-addr-empty",
            "服务器地址不能为空",
            None,
        ));
    }
    Ok(server_addr)
}

fn validate_disk_name(disk_name: &str) -> Result<&str, ApiError> {
    let disk_name = disk_name.trim();
    if disk_name.is_empty() {
        return Err(ApiError::new("invalid-disk-name", "磁盘名称不能为空", None));
    }
    Ok(disk_name)
}

fn lock_network_client(
    network_client_mutex: &Mutex<NetworkClientState>,
) -> std::sync::MutexGuard<'_, NetworkClientState> {
    network_client_mutex
        .lock()
        .expect("network client mutex should not be poisoned")
}

fn draft_not_found_error(draft_id: &str) -> ApiError {
    ApiError::new(
        "network-draft-not-found",
        "网络盘草稿不存在",
        Some(draft_id.to_string()),
    )
}

fn draft_item_not_found_error(remote_disk_id: &str) -> ApiError {
    ApiError::new(
        "network-draft-item-not-found",
        "网络盘草稿项不存在",
        Some(remote_disk_id.to_string()),
    )
}

fn disk_not_found_error(local_disk_id: &str) -> ApiError {
    ApiError::new(
        "disk-not-found",
        "磁盘不存在",
        Some(local_disk_id.to_string()),
    )
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

fn map_mount_error(error: NetworkClientError) -> ApiError {
    ApiError::new("mount-disk-failed", "挂载磁盘失败", Some(error.to_string()))
}
