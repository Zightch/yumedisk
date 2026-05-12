use serde::Serialize;
use tauri::State;

use crate::backend::disk_service;
use crate::state::client_state::ClientState;
use crate::state::disk_store::DiskMediaConfig;
use crate::state::disk_store::FileMediaKind;
use crate::state::disk_store::MemoryMediaKind;

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

#[derive(Debug, Clone, Serialize)]
#[serde(rename_all = "camelCase")]
pub enum MemoryMediaKindDto {
    DenseMem,
    SparseMem,
}

#[derive(Debug, Clone, Serialize)]
#[serde(rename_all = "camelCase")]
pub enum FileMediaKindDto {
    RawFile,
}

#[derive(Debug, Clone, Serialize)]
#[serde(tag = "kind", rename_all = "camelCase")]
pub enum HomeDiskMediaDto {
    Memory {
        #[serde(rename = "memoryKind")]
        memory_kind: MemoryMediaKindDto,
        #[serde(rename = "capacityBytes")]
        capacity_bytes: u64,
    },
    File {
        #[serde(rename = "fileKind")]
        file_kind: FileMediaKindDto,
        #[serde(rename = "filePath")]
        file_path: String,
        #[serde(rename = "capacityBytes")]
        capacity_bytes: u64,
    },
}

#[derive(Debug, Clone, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct HomeDiskListItemDto {
    pub disk_id: String,
    pub disk_name: String,
    pub auto_connect: bool,
    pub read_only: bool,
    pub connected: bool,
    pub online: bool,
    pub target_id: Option<u32>,
    pub lifecycle_text: String,
    pub visible_path: String,
    pub physical_drive_path: String,
    pub media: HomeDiskMediaDto,
}

#[derive(Debug, Clone, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct QueryHomeDiskListResponse {
    pub disks: Vec<HomeDiskListItemDto>,
    pub auto_connect_count: u32,
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

fn map_home_disk_media_dto(media: DiskMediaConfig) -> HomeDiskMediaDto {
    match media {
        DiskMediaConfig::Memory {
            memory_kind,
            capacity_bytes,
        } => HomeDiskMediaDto::Memory {
            memory_kind: match memory_kind {
                MemoryMediaKind::DenseMem => MemoryMediaKindDto::DenseMem,
                MemoryMediaKind::SparseMem => MemoryMediaKindDto::SparseMem,
            },
            capacity_bytes,
        },
        DiskMediaConfig::File {
            file_kind,
            file_path,
            capacity_bytes,
        } => HomeDiskMediaDto::File {
            file_kind: match file_kind {
                FileMediaKind::RawFile => FileMediaKindDto::RawFile,
            },
            file_path,
            capacity_bytes,
        },
    }
}

fn map_home_disk_list_item_dto(
    snapshot: disk_service::HomeDiskListItemSnapshot,
) -> HomeDiskListItemDto {
    HomeDiskListItemDto {
        disk_id: snapshot.disk_id,
        disk_name: snapshot.disk_name,
        auto_connect: snapshot.auto_connect,
        read_only: snapshot.read_only,
        connected: snapshot.connected,
        online: snapshot.online,
        target_id: snapshot.target_id,
        lifecycle_text: snapshot.lifecycle_text,
        visible_path: snapshot.visible_path,
        physical_drive_path: snapshot.physical_drive_path,
        media: map_home_disk_media_dto(snapshot.media),
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

#[tauri::command]
pub fn query_home_disk_list(state: State<'_, ClientState>) -> QueryHomeDiskListResponse {
    let snapshot = {
        let disk_store = state
            .disk_store
            .lock()
            .expect("disk store mutex should not be poisoned");

        disk_service::query_home_disk_list(&state.backend, &disk_store)
    };

    let auto_connect_count = snapshot.disks.iter().filter(|disk| disk.auto_connect).count() as u32;
    let disks = snapshot
        .disks
        .into_iter()
        .map(map_home_disk_list_item_dto)
        .collect();

    QueryHomeDiskListResponse {
        disks,
        auto_connect_count,
    }
}
