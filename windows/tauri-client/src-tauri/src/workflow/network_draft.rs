use std::sync::Mutex;

use backend_rust::BackendContext;

use crate::api_error::ApiError;
use crate::network::draft_flow;
use crate::network::runtime_flow;
use crate::state::disk_runtime::DiskRuntimeStore;
use crate::state::network_client::NetworkClientState;

use super::network_runtime;

pub fn add_network_draft_item(
    backend: &BackendContext,
    runtime_store: &mut DiskRuntimeStore,
    network_client_mutex: &Mutex<NetworkClientState>,
    request: draft_flow::AddNetworkDraftItemRequest,
) -> Result<draft_flow::NetworkCreateDraftSnapshot, ApiError> {
    network_runtime::sync_runtime_state(backend, runtime_store, network_client_mutex);
    draft_flow::add_network_draft_item(runtime_store, network_client_mutex, request)
}

pub fn submit_network_draft(
    backend: &BackendContext,
    runtime_store: &mut DiskRuntimeStore,
    network_client_mutex: &Mutex<NetworkClientState>,
    request: draft_flow::SubmitNetworkDraftRequest,
) -> Result<(), ApiError> {
    network_runtime::sync_runtime_state(backend, runtime_store, network_client_mutex);
    draft_flow::submit_network_draft(backend, runtime_store, network_client_mutex, request)?;
    runtime_flow::rescan_network_runtimes(backend, runtime_store, network_client_mutex);
    Ok(())
}
