use serde::Deserialize;
use serde::Serialize;
use tauri::State;

use crate::api_error::ApiError;
use crate::network::draft_flow;
use crate::state::client_state::ClientState;
use crate::workflow::network_draft;

#[derive(Debug, Clone, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct TestNetworkConnectionRequestDto {
    pub server_addr: String,
}

#[derive(Debug, Clone, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct CreateNetworkDraftRequestDto {
    pub server_addr: String,
}

#[derive(Debug, Clone, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct AddNetworkDraftItemRequestDto {
    pub draft_id: String,
    pub disk_name: String,
    pub claim_code: String,
}

#[derive(Debug, Clone, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct RemoveNetworkDraftItemRequestDto {
    pub draft_id: String,
    pub remote_disk_id: String,
}

#[derive(Debug, Clone, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct SubmitNetworkDraftRequestDto {
    pub draft_id: String,
}

#[derive(Debug, Clone, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct DisposeNetworkDraftRequestDto {
    pub draft_id: String,
}

#[derive(Debug, Clone, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct NetworkDraftItemDto {
    pub disk_name: String,
    pub server_addr: String,
    pub remote_disk_id: String,
    pub capacity_bytes: u64,
    pub read_only: bool,
}

#[derive(Debug, Clone, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct NetworkDraftSnapshotDto {
    pub draft_id: String,
    pub server_addr: String,
    pub items: Vec<NetworkDraftItemDto>,
}

fn map_network_draft_snapshot(
    snapshot: draft_flow::NetworkCreateDraftSnapshot,
) -> NetworkDraftSnapshotDto {
    NetworkDraftSnapshotDto {
        draft_id: snapshot.draft_id,
        server_addr: snapshot.server_addr,
        items: snapshot
            .items
            .into_iter()
            .map(|item| NetworkDraftItemDto {
                disk_name: item.disk_name,
                server_addr: item.server_addr,
                remote_disk_id: item.remote_disk_id,
                capacity_bytes: item.capacity_bytes,
                read_only: item.read_only,
            })
            .collect(),
    }
}

#[tauri::command]
pub fn test_network_connection(
    state: State<'_, ClientState>,
    request: TestNetworkConnectionRequestDto,
) -> Result<(), ApiError> {
    draft_flow::test_connection(&state.network_client, &request.server_addr)
}

#[tauri::command]
pub fn create_network_draft(
    state: State<'_, ClientState>,
    request: CreateNetworkDraftRequestDto,
) -> Result<NetworkDraftSnapshotDto, ApiError> {
    draft_flow::create_network_draft(
        &state.network_client,
        draft_flow::CreateNetworkDraftRequest {
            server_addr: request.server_addr,
        },
    )
    .map(map_network_draft_snapshot)
}

#[tauri::command]
pub fn add_network_draft_item(
    state: State<'_, ClientState>,
    request: AddNetworkDraftItemRequestDto,
) -> Result<NetworkDraftSnapshotDto, ApiError> {
    let mut disk_catalog = state
        .disk_catalog
        .lock()
        .expect("disk catalog mutex should not be poisoned");
    network_draft::add_network_draft_item(
        &state.backend,
        disk_catalog.runtime_store_mut(),
        &state.network_client,
        draft_flow::AddNetworkDraftItemRequest {
            draft_id: request.draft_id,
            disk_name: request.disk_name,
            claim_code: request.claim_code,
        },
    )
    .map(map_network_draft_snapshot)
}

#[tauri::command]
pub fn remove_network_draft_item(
    state: State<'_, ClientState>,
    request: RemoveNetworkDraftItemRequestDto,
) -> Result<NetworkDraftSnapshotDto, ApiError> {
    draft_flow::remove_network_draft_item(
        &state.network_client,
        draft_flow::RemoveNetworkDraftItemRequest {
            draft_id: request.draft_id,
            remote_disk_id: request.remote_disk_id,
        },
    )
    .map(map_network_draft_snapshot)
}

#[tauri::command]
pub fn submit_network_draft(
    state: State<'_, ClientState>,
    request: SubmitNetworkDraftRequestDto,
) -> Result<(), ApiError> {
    let mut disk_catalog = state
        .disk_catalog
        .lock()
        .expect("disk catalog mutex should not be poisoned");
    network_draft::submit_network_draft(
        &state.backend,
        disk_catalog.runtime_store_mut(),
        &state.network_client,
        draft_flow::SubmitNetworkDraftRequest {
            draft_id: request.draft_id,
        },
    )
}

#[tauri::command]
pub fn dispose_network_draft(
    state: State<'_, ClientState>,
    request: DisposeNetworkDraftRequestDto,
) -> Result<(), ApiError> {
    draft_flow::dispose_network_draft(
        &state.network_client,
        draft_flow::DisposeNetworkDraftRequest {
            draft_id: request.draft_id,
        },
    )
}
