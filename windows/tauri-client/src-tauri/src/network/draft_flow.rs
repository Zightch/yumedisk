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
use super::runtime_flow;
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

    runtime_flow::rescan_network_runtimes(runtime_store, network_client_mutex);
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
