use serde::Serialize;
use tauri::State;

use crate::backend::disk_service;
use crate::state::client_state::ClientState;

#[derive(Debug, Clone, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct ManagedDiskSnapshotDto {
    pub target_id: u32,
    pub disk_size_bytes: u64,
    pub sector_size: u32,
    pub read_only: bool,
    pub visible_path: String,
    pub physical_drive_path: String,
    pub lifecycle_text: String,
    pub online: bool,
}

#[derive(Debug, Clone, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct QueryManagedDisksResponse {
    pub disks: Vec<ManagedDiskSnapshotDto>,
}

fn map_managed_disk_snapshot_dto(
    snapshot: backend_rust::ManagedDiskSnapshot,
) -> ManagedDiskSnapshotDto {
    ManagedDiskSnapshotDto {
        target_id: snapshot.target_id,
        disk_size_bytes: snapshot.disk_size_bytes,
        sector_size: snapshot.sector_size,
        read_only: snapshot.read_only,
        visible_path: snapshot.visible_path,
        physical_drive_path: snapshot.physical_drive_path,
        lifecycle_text: snapshot.lifecycle_text,
        online: snapshot.online,
    }
}

#[tauri::command]
pub fn query_managed_disks(state: State<'_, ClientState>) -> QueryManagedDisksResponse {
    let disks = disk_service::query_managed_disks(&state.backend)
        .into_iter()
        .map(map_managed_disk_snapshot_dto)
        .collect();

    QueryManagedDisksResponse { disks }
}
