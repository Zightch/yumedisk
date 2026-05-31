use std::sync::Mutex;

use backend_rust::BackendContext;

use crate::api_error::ApiError;
use crate::backend::disk_service;
use crate::network::runtime_flow;
use crate::state::disk_runtime::DiskRuntimeStore;
use crate::state::disk_runtime::RemovedDiskRuntime;
use crate::state::network_client::NetworkClientState;

use super::network_runtime;

pub fn query_home_disk_list(
    backend: &BackendContext,
    runtime_store: &mut DiskRuntimeStore,
    network_client_mutex: &Mutex<NetworkClientState>,
) -> disk_service::HomeDiskListSnapshot {
    network_runtime::sync_runtime_state(backend, runtime_store, network_client_mutex);
    disk_service::query_home_disk_list(backend, runtime_store)
}

pub fn rescan_local_runtime_disks(
    backend: &BackendContext,
    runtime_store: &mut DiskRuntimeStore,
    network_client_mutex: &Mutex<NetworkClientState>,
) {
    network_runtime::sync_runtime_state(backend, runtime_store, network_client_mutex);
    disk_service::rescan_local_runtime_disks(backend, runtime_store);
}

pub fn mount_disk(
    backend: &BackendContext,
    runtime_store: &mut DiskRuntimeStore,
    network_client_mutex: &Mutex<NetworkClientState>,
    local_disk_id: &str,
) -> Result<u32, ApiError> {
    network_runtime::sync_runtime_state(backend, runtime_store, network_client_mutex);

    if runtime_store
        .find_runtime(local_disk_id)
        .ok_or_else(|| {
            ApiError::new(
                "disk-not-found",
                "磁盘不存在",
                Some(local_disk_id.to_string()),
            )
        })?
        .is_network()
    {
        return runtime_flow::mount_network_disk(
            backend,
            runtime_store,
            network_client_mutex,
            local_disk_id,
        );
    }

    disk_service::mount_local_disk(backend, runtime_store, local_disk_id)
}

pub fn eject_disk(
    backend: &BackendContext,
    runtime_store: &mut DiskRuntimeStore,
    network_client_mutex: &Mutex<NetworkClientState>,
    local_disk_id: &str,
) -> Result<(), ApiError> {
    network_runtime::sync_runtime_state(backend, runtime_store, network_client_mutex);

    if runtime_store
        .find_runtime(local_disk_id)
        .ok_or_else(|| {
            ApiError::new(
                "disk-not-found",
                "磁盘不存在",
                Some(local_disk_id.to_string()),
            )
        })?
        .is_network()
    {
        return runtime_flow::eject_network_disk(backend, runtime_store, local_disk_id);
    }

    disk_service::eject_local_disk(backend, runtime_store, local_disk_id)
}

pub fn prepare_deleted_runtime(
    backend: &BackendContext,
    removed_runtime: &mut RemovedDiskRuntime,
    network_client_mutex: &Mutex<NetworkClientState>,
) -> Result<(), ApiError> {
    if removed_runtime.runtime.is_network() {
        runtime_flow::prepare_deleted_network_runtime(
            backend,
            removed_runtime,
            network_client_mutex,
        )
    } else {
        disk_service::prepare_deleted_local_runtime(backend, removed_runtime)
    }
}
