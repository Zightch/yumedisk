use serde::Deserialize;
use serde::Serialize;
use tauri::State;

use crate::api_error::ApiError;
use crate::backend::disk_service;
use crate::backend::persistence_service;
use crate::state::client_state::ClientState;
use crate::state::disk_runtime::DiskMediaConfig;
use crate::state::disk_runtime::DiskRuntimeStatus;
use crate::state::disk_runtime::FileMediaKind;
use crate::state::disk_runtime::MemoryMediaKind;

#[derive(Debug, Clone, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct ManagedDiskSnapshotDto {
    pub target_id: u32,
    pub disk_size_bytes: u64,
    pub sector_size: u32,
    pub read_only: bool,
    pub lifecycle_text: String,
    pub online: bool,
}

#[derive(Debug, Clone, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct QueryManagedDisksResponse {
    pub disks: Vec<ManagedDiskSnapshotDto>,
}

#[derive(Debug, Clone, Deserialize)]
#[serde(rename_all = "camelCase")]
pub enum RequestedMemoryMediaKindDto {
    Auto,
    DenseMem,
    SparseMem,
}

#[derive(Debug, Clone, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct CreateMemoryDiskRequestDto {
    pub disk_name: String,
    #[serde(rename = "capacityMiB")]
    pub capacity_mib: u64,
    pub requested_memory_kind: RequestedMemoryMediaKindDto,
    pub auto_connect: bool,
}

#[derive(Debug, Clone, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct CreateFileDiskRequestDto {
    pub disk_name: String,
    pub file_path: String,
    pub auto_connect: bool,
}

#[derive(Debug, Clone, Deserialize)]
#[serde(rename_all = "camelCase")]
pub enum CreateFileFormatDto {
    Raw,
    Vmdk,
    Vhd,
    Vhdx,
    Vdi,
    Qcow2,
}

#[derive(Debug, Clone, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct CreateNewFileDiskRequestDto {
    pub disk_name: String,
    pub file_path: String,
    #[serde(rename = "capacityMiB")]
    pub capacity_mib: u64,
    pub file_format: CreateFileFormatDto,
    pub auto_connect: bool,
}

#[derive(Debug, Clone, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct CreateMemoryDiskResponse {
    pub disk_id: String,
}

#[derive(Debug, Clone, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct PickRawFilePathResponse {
    pub file_path: Option<String>,
}

#[derive(Debug, Clone, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct CreateFileDiskResponse {
    pub disk_id: String,
}

#[derive(Debug, Clone, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct ConnectDiskRequestDto {
    pub disk_id: String,
}

#[derive(Debug, Clone, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct ConnectDiskResponse {
    pub target_id: u32,
}

#[derive(Debug, Clone, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct DisconnectDiskRequestDto {
    pub disk_id: String,
}

#[derive(Debug, Clone, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct DeleteDiskRequestDto {
    pub disk_id: String,
}

#[derive(Debug, Clone, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct UpdateDiskRequestDto {
    pub disk_id: String,
    pub disk_name: String,
    pub auto_connect: bool,
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
#[serde(rename_all = "camelCase")]
pub enum DiskStatusDto {
    Disconnected,
    Connected,
    Invalid,
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
    pub status: DiskStatusDto,
    pub invalid_reason: Option<String>,
    pub online: bool,
    pub target_id: Option<u32>,
    pub lifecycle_text: String,
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
        status: match snapshot.status {
            DiskRuntimeStatus::Disconnected => DiskStatusDto::Disconnected,
            DiskRuntimeStatus::Connected { .. } => DiskStatusDto::Connected,
            DiskRuntimeStatus::Invalid { .. } => DiskStatusDto::Invalid,
        },
        invalid_reason: snapshot.invalid_reason,
        online: snapshot.online,
        target_id: snapshot.target_id,
        lifecycle_text: snapshot.lifecycle_text,
        media: map_home_disk_media_dto(snapshot.media),
    }
}

fn map_requested_memory_media_kind(
    requested_kind: RequestedMemoryMediaKindDto,
) -> disk_service::RequestedMemoryMediaKind {
    match requested_kind {
        RequestedMemoryMediaKindDto::Auto => disk_service::RequestedMemoryMediaKind::Auto,
        RequestedMemoryMediaKindDto::DenseMem => disk_service::RequestedMemoryMediaKind::DenseMem,
        RequestedMemoryMediaKindDto::SparseMem => disk_service::RequestedMemoryMediaKind::SparseMem,
    }
}

fn map_create_file_format(file_format: CreateFileFormatDto) -> disk_service::CreateFileFormat {
    match file_format {
        CreateFileFormatDto::Raw => disk_service::CreateFileFormat::Raw,
        CreateFileFormatDto::Vmdk => disk_service::CreateFileFormat::Vmdk,
        CreateFileFormatDto::Vhd => disk_service::CreateFileFormat::Vhd,
        CreateFileFormatDto::Vhdx => disk_service::CreateFileFormat::Vhdx,
        CreateFileFormatDto::Vdi => disk_service::CreateFileFormat::Vdi,
        CreateFileFormatDto::Qcow2 => disk_service::CreateFileFormat::Qcow2,
    }
}

fn build_home_disk_list_response(
    snapshot: disk_service::HomeDiskListSnapshot,
) -> QueryHomeDiskListResponse {
    let auto_connect_count = snapshot
        .disks
        .iter()
        .filter(|disk| disk.auto_connect)
        .count() as u32;
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
        let disk_runtime_store = state
            .disk_runtime_store
            .lock()
            .expect("disk runtime store mutex should not be poisoned");

        disk_service::query_home_disk_list(&state.backend, &disk_runtime_store)
    };

    build_home_disk_list_response(snapshot)
}

#[tauri::command]
pub fn rescan_runtime_disks(
    state: State<'_, ClientState>,
) -> Result<QueryHomeDiskListResponse, ApiError> {
    let snapshot = {
        let mut disk_runtime_store = state
            .disk_runtime_store
            .lock()
            .expect("disk runtime store mutex should not be poisoned");

        let snapshot = disk_service::rescan_runtime_disks(&state.backend, &mut disk_runtime_store);
        persistence_service::save_client_state(&state.backend, &disk_runtime_store)?;
        snapshot
    };

    Ok(build_home_disk_list_response(snapshot))
}

#[tauri::command]
pub fn create_memory_disk(
    state: State<'_, ClientState>,
    request: CreateMemoryDiskRequestDto,
) -> Result<CreateMemoryDiskResponse, ApiError> {
    let disk_id = {
        let mut disk_runtime_store = state
            .disk_runtime_store
            .lock()
            .expect("disk runtime store mutex should not be poisoned");

        let disk_id = disk_service::create_memory_disk(
            &mut disk_runtime_store,
            disk_service::CreateMemoryDiskRequest {
                disk_name: request.disk_name,
                capacity_mib: request.capacity_mib,
                requested_memory_kind: map_requested_memory_media_kind(
                    request.requested_memory_kind,
                ),
                auto_connect: request.auto_connect,
            },
        )?;
        if let Err(error) =
            persistence_service::save_client_state(&state.backend, &disk_runtime_store)
        {
            let _ = disk_runtime_store.remove_runtime(&disk_id);
            return Err(error);
        }
        disk_id
    };

    Ok(CreateMemoryDiskResponse { disk_id })
}

#[tauri::command]
pub fn pick_raw_file_path() -> PickRawFilePathResponse {
    PickRawFilePathResponse {
        file_path: disk_service::pick_raw_file_path(),
    }
}

#[tauri::command]
pub fn pick_new_raw_file_path() -> PickRawFilePathResponse {
    PickRawFilePathResponse {
        file_path: disk_service::pick_new_raw_file_path(),
    }
}

#[tauri::command]
pub fn create_file_disk(
    state: State<'_, ClientState>,
    request: CreateFileDiskRequestDto,
) -> Result<CreateFileDiskResponse, ApiError> {
    let disk_id = {
        let mut disk_runtime_store = state
            .disk_runtime_store
            .lock()
            .expect("disk runtime store mutex should not be poisoned");

        let disk_id = disk_service::create_file_disk(
            &mut disk_runtime_store,
            disk_service::CreateFileDiskRequest {
                disk_name: request.disk_name,
                file_path: request.file_path,
                auto_connect: request.auto_connect,
            },
        )?;
        if let Err(error) =
            persistence_service::save_client_state(&state.backend, &disk_runtime_store)
        {
            let _ = disk_runtime_store.remove_runtime(&disk_id);
            return Err(error);
        }
        disk_id
    };

    Ok(CreateFileDiskResponse { disk_id })
}

#[tauri::command]
pub fn create_new_file_disk(
    state: State<'_, ClientState>,
    request: CreateNewFileDiskRequestDto,
) -> Result<CreateFileDiskResponse, ApiError> {
    let disk_id = {
        let mut disk_runtime_store = state
            .disk_runtime_store
            .lock()
            .expect("disk runtime store mutex should not be poisoned");

        let disk_id = disk_service::create_new_file_disk(
            &mut disk_runtime_store,
            disk_service::CreateNewFileDiskRequest {
                disk_name: request.disk_name,
                file_path: request.file_path,
                capacity_mib: request.capacity_mib,
                file_format: map_create_file_format(request.file_format),
                auto_connect: request.auto_connect,
            },
        )?;
        if let Err(error) =
            persistence_service::save_client_state(&state.backend, &disk_runtime_store)
        {
            let _ = disk_runtime_store.remove_runtime(&disk_id);
            return Err(error);
        }
        disk_id
    };

    Ok(CreateFileDiskResponse { disk_id })
}

#[tauri::command]
pub fn connect_disk(
    state: State<'_, ClientState>,
    request: ConnectDiskRequestDto,
) -> Result<ConnectDiskResponse, ApiError> {
    let target_id = {
        let mut disk_runtime_store = state
            .disk_runtime_store
            .lock()
            .expect("disk runtime store mutex should not be poisoned");

        disk_service::connect_disk(&state.backend, &mut disk_runtime_store, &request.disk_id)?
    };

    Ok(ConnectDiskResponse { target_id })
}

#[tauri::command]
pub fn disconnect_disk(
    state: State<'_, ClientState>,
    request: DisconnectDiskRequestDto,
) -> Result<(), ApiError> {
    let mut disk_runtime_store = state
        .disk_runtime_store
        .lock()
        .expect("disk runtime store mutex should not be poisoned");

    disk_service::disconnect_disk(&state.backend, &mut disk_runtime_store, &request.disk_id)
}

#[tauri::command]
pub fn delete_disk(
    state: State<'_, ClientState>,
    request: DeleteDiskRequestDto,
) -> Result<(), ApiError> {
    let mut disk_runtime_store = state
        .disk_runtime_store
        .lock()
        .expect("disk runtime store mutex should not be poisoned");

    let deleted_state =
        disk_service::delete_disk(&state.backend, &mut disk_runtime_store, &request.disk_id)?;

    if let Err(error) = persistence_service::save_client_state(&state.backend, &disk_runtime_store)
    {
        disk_runtime_store.restore_removed_runtime(deleted_state.removed_runtime);
        return Err(error);
    }

    Ok(())
}

#[tauri::command]
pub fn update_disk(
    state: State<'_, ClientState>,
    request: UpdateDiskRequestDto,
) -> Result<(), ApiError> {
    let mut disk_runtime_store = state
        .disk_runtime_store
        .lock()
        .expect("disk runtime store mutex should not be poisoned");

    let updated_state = disk_service::update_disk(
        &mut disk_runtime_store,
        disk_service::UpdateDiskRequest {
            disk_id: request.disk_id,
            disk_name: request.disk_name,
            auto_connect: request.auto_connect,
        },
    )?;

    if let Err(error) = persistence_service::save_client_state(&state.backend, &disk_runtime_store)
    {
        let runtime = disk_runtime_store
            .find_runtime_mut(&updated_state.previous_snapshot.disk_id)
            .ok_or_else(|| {
                ApiError::new(
                    "disk-not-found",
                    "磁盘不存在",
                    Some(updated_state.previous_snapshot.disk_id.clone()),
                )
            })?;
        runtime.set_identity(
            updated_state.previous_snapshot.disk_name,
            updated_state.previous_snapshot.auto_connect,
        );
        return Err(error);
    }

    Ok(())
}
