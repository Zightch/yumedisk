use serde::Deserialize;
use serde::Serialize;
use tauri::{AppHandle, State};

use crate::api_error::ApiError;
use crate::backend::disk_service;
use crate::backend::persistence_service;
use crate::state::client_state::ClientState;
use crate::state::disk_catalog::PendingDiskDeletion;
use crate::state::disk_runtime::DiskMediaConfig;
use crate::state::disk_runtime::DiskRuntimeStatus;
use crate::state::disk_runtime::FileMediaKind;
use crate::state::disk_runtime::MemoryMediaKind;
use crate::workflow::network_runtime;
use crate::workflow::runtime_disk;
use crate::workflow::runtime_rescan;

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

#[derive(Debug, Clone, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct RuntimeRescanStateResponse {
    pub running: bool,
}

#[derive(Debug, Clone, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct RuntimeRescanStartResponse {
    pub started: bool,
    pub running: bool,
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
    pub auto_mount: bool,
}

#[derive(Debug, Clone, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct CreateFileDiskRequestDto {
    pub disk_name: String,
    pub file_path: String,
    pub auto_mount: bool,
    pub configured_read_only: bool,
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
    pub auto_mount: bool,
}

#[derive(Debug, Clone, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct CreateMemoryDiskResponse {
    pub local_disk_id: String,
}

#[derive(Debug, Clone, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct PickRawFilePathResponse {
    pub file_path: Option<String>,
}

#[derive(Debug, Clone, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct CreateFileDiskResponse {
    pub local_disk_id: String,
}

#[derive(Debug, Clone, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct MountDiskRequestDto {
    pub local_disk_id: String,
}

#[derive(Debug, Clone, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct MountDiskResponse {
    pub target_id: u32,
}

#[derive(Debug, Clone, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct EjectDiskRequestDto {
    pub local_disk_id: String,
}

#[derive(Debug, Clone, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct DeleteDiskRequestDto {
    pub local_disk_id: String,
}

#[derive(Debug, Clone, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct UndoDeleteDiskRequestDto {
    pub deletion_id: String,
}

#[derive(Debug, Clone, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct CommitDeletedDiskRequestDto {
    pub deletion_id: String,
}

#[derive(Debug, Clone, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct UpdateDiskRequestDto {
    pub local_disk_id: String,
    pub disk_name: String,
    pub auto_mount: bool,
    pub configured_read_only: bool,
}

#[derive(Debug, Clone, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct DeleteDiskResponse {
    pub deletion_id: String,
    pub undo_available: bool,
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
    Unmounted,
    Mounted,
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
    Network {
        #[serde(rename = "serverAddr")]
        server_addr: String,
        #[serde(rename = "remoteDiskId")]
        remote_disk_id: String,
        #[serde(rename = "capacityBytes")]
        capacity_bytes: u64,
    },
}

#[derive(Debug, Clone, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct HomeDiskListItemDto {
    pub local_disk_id: String,
    pub disk_name: String,
    pub auto_mount: bool,
    pub configured_read_only: bool,
    pub source_read_only: bool,
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
    pub auto_mount_count: u32,
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
        DiskMediaConfig::Network {
            server_addr,
            remote_disk_id,
            capacity_bytes,
            ..
        } => HomeDiskMediaDto::Network {
            server_addr,
            remote_disk_id,
            capacity_bytes,
        },
    }
}

fn map_home_disk_list_item_dto(
    snapshot: disk_service::HomeDiskListItemSnapshot,
) -> HomeDiskListItemDto {
    HomeDiskListItemDto {
        local_disk_id: snapshot.local_disk_id,
        disk_name: snapshot.disk_name,
        auto_mount: snapshot.auto_mount,
        configured_read_only: snapshot.configured_read_only,
        source_read_only: snapshot.source_read_only,
        status: match snapshot.status {
            DiskRuntimeStatus::Unmounted => DiskStatusDto::Unmounted,
            DiskRuntimeStatus::Mounted { .. } => DiskStatusDto::Mounted,
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
    let auto_mount_count = snapshot.disks.iter().filter(|disk| disk.auto_mount).count() as u32;
    let disks = snapshot
        .disks
        .into_iter()
        .map(map_home_disk_list_item_dto)
        .collect();

    QueryHomeDiskListResponse {
        disks,
        auto_mount_count,
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
        let mut disk_catalog = state
            .disk_catalog
            .lock()
            .expect("disk catalog mutex should not be poisoned");
        if state.is_runtime_rescan_running() {
            disk_service::query_home_disk_list(&state.backend, disk_catalog.runtime_store())
        } else {
            runtime_disk::query_home_disk_list(
                &state.backend,
                disk_catalog.runtime_store_mut(),
                &state.network_client,
            )
        }
    };

    build_home_disk_list_response(snapshot)
}

#[tauri::command]
pub fn rescan_runtime_disks(
    app: AppHandle,
) -> Result<RuntimeRescanStartResponse, ApiError> {
    runtime_rescan::start_runtime_rescan(app).map(|result| RuntimeRescanStartResponse {
        started: result.started,
        running: result.running,
    })
}

#[tauri::command]
pub fn query_runtime_rescan_state(state: State<'_, ClientState>) -> RuntimeRescanStateResponse {
    let snapshot = runtime_rescan::query_runtime_rescan_state(&state);
    RuntimeRescanStateResponse {
        running: snapshot.running,
    }
}

#[tauri::command]
pub fn create_memory_disk(
    state: State<'_, ClientState>,
    request: CreateMemoryDiskRequestDto,
) -> Result<CreateMemoryDiskResponse, ApiError> {
    runtime_rescan::ensure_runtime_rescan_idle(&state)?;

    let local_disk_id = {
        let mut disk_catalog = state
            .disk_catalog
            .lock()
            .expect("disk catalog mutex should not be poisoned");

        let local_disk_id = disk_service::create_memory_disk(
            disk_catalog.runtime_store_mut(),
            disk_service::CreateMemoryDiskRequest {
                disk_name: request.disk_name,
                capacity_mib: request.capacity_mib,
                requested_memory_kind: map_requested_memory_media_kind(
                    request.requested_memory_kind,
                ),
                auto_mount: request.auto_mount,
            },
        )?;
        if let Err(error) =
            persistence_service::save_client_state(&state.backend, disk_catalog.runtime_store())
        {
            let _ = disk_catalog.remove_runtime(&local_disk_id);
            return Err(error);
        }
        local_disk_id
    };

    Ok(CreateMemoryDiskResponse { local_disk_id })
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
    runtime_rescan::ensure_runtime_rescan_idle(&state)?;

    let local_disk_id = {
        let mut disk_catalog = state
            .disk_catalog
            .lock()
            .expect("disk catalog mutex should not be poisoned");

        let local_disk_id = disk_service::create_file_disk(
            disk_catalog.runtime_store_mut(),
            disk_service::CreateFileDiskRequest {
                disk_name: request.disk_name,
                file_path: request.file_path,
                auto_mount: request.auto_mount,
                configured_read_only: request.configured_read_only,
            },
        )?;
        if let Err(error) =
            persistence_service::save_client_state(&state.backend, disk_catalog.runtime_store())
        {
            let _ = disk_catalog.remove_runtime(&local_disk_id);
            return Err(error);
        }
        local_disk_id
    };

    Ok(CreateFileDiskResponse { local_disk_id })
}

#[tauri::command]
pub fn create_new_file_disk(
    state: State<'_, ClientState>,
    request: CreateNewFileDiskRequestDto,
) -> Result<CreateFileDiskResponse, ApiError> {
    runtime_rescan::ensure_runtime_rescan_idle(&state)?;

    let local_disk_id = {
        let mut disk_catalog = state
            .disk_catalog
            .lock()
            .expect("disk catalog mutex should not be poisoned");

        let local_disk_id = disk_service::create_new_file_disk(
            disk_catalog.runtime_store_mut(),
            disk_service::CreateNewFileDiskRequest {
                disk_name: request.disk_name,
                file_path: request.file_path,
                capacity_mib: request.capacity_mib,
                file_format: map_create_file_format(request.file_format),
                auto_mount: request.auto_mount,
            },
        )?;
        if let Err(error) =
            persistence_service::save_client_state(&state.backend, disk_catalog.runtime_store())
        {
            let _ = disk_catalog.remove_runtime(&local_disk_id);
            return Err(error);
        }
        local_disk_id
    };

    Ok(CreateFileDiskResponse { local_disk_id })
}

#[tauri::command]
pub fn mount_disk(
    state: State<'_, ClientState>,
    request: MountDiskRequestDto,
) -> Result<MountDiskResponse, ApiError> {
    runtime_rescan::ensure_runtime_rescan_idle(&state)?;

    let target_id = {
        let mut disk_catalog = state
            .disk_catalog
            .lock()
            .expect("disk catalog mutex should not be poisoned");
        runtime_disk::mount_disk(
            &state.backend,
            disk_catalog.runtime_store_mut(),
            &state.network_client,
            &request.local_disk_id,
        )?
    };

    Ok(MountDiskResponse { target_id })
}

#[tauri::command]
pub fn eject_disk(
    state: State<'_, ClientState>,
    request: EjectDiskRequestDto,
) -> Result<(), ApiError> {
    runtime_rescan::ensure_runtime_rescan_idle(&state)?;

    let mut disk_catalog = state
        .disk_catalog
        .lock()
        .expect("disk catalog mutex should not be poisoned");
    runtime_disk::eject_disk(
        &state.backend,
        disk_catalog.runtime_store_mut(),
        &state.network_client,
        &request.local_disk_id,
    )
}

#[tauri::command]
pub fn delete_disk(
    state: State<'_, ClientState>,
    request: DeleteDiskRequestDto,
) -> Result<DeleteDiskResponse, ApiError> {
    runtime_rescan::ensure_runtime_rescan_idle(&state)?;

    let mut disk_catalog = state
        .disk_catalog
        .lock()
        .expect("disk catalog mutex should not be poisoned");
    network_runtime::sync_runtime_state(
        &state.backend,
        disk_catalog.runtime_store_mut(),
        &state.network_client,
    );

    let Some(mut removed_runtime) = disk_catalog.remove_runtime(&request.local_disk_id) else {
        return Err(ApiError::new(
            "disk-not-found",
            "磁盘不存在",
            Some(request.local_disk_id),
        ));
    };

    if let Err(error) = runtime_disk::prepare_deleted_runtime(
        &state.backend,
        &mut removed_runtime,
        &state.network_client,
    ) {
        disk_catalog.restore_removed_runtime(removed_runtime);
        return Err(error);
    }

    if let Err(error) =
        persistence_service::save_client_state(&state.backend, disk_catalog.runtime_store())
    {
        disk_catalog.restore_removed_runtime(removed_runtime);
        return Err(error);
    }

    let deletion_id = disk_catalog.stage_pending_deletion(removed_runtime);
    Ok(DeleteDiskResponse {
        deletion_id,
        undo_available: true,
    })
}

#[tauri::command]
pub fn undo_delete_disk(
    state: State<'_, ClientState>,
    request: UndoDeleteDiskRequestDto,
) -> Result<(), ApiError> {
    runtime_rescan::ensure_runtime_rescan_idle(&state)?;

    let mut disk_catalog = state
        .disk_catalog
        .lock()
        .expect("disk catalog mutex should not be poisoned");

    let pending_deletion = disk_catalog
        .take_pending_deletion(&request.deletion_id)
        .ok_or_else(|| {
            ApiError::new(
                "pending-deletion-not-found",
                "删除撤销窗口已结束",
                Some(request.deletion_id.clone()),
            )
        })?;
    let local_disk_id = pending_deletion
        .removed_runtime
        .runtime
        .local_disk_id()
        .to_string();
    let deletion_id = pending_deletion.deletion_id.clone();

    disk_catalog.restore_removed_runtime(pending_deletion.removed_runtime);

    if let Err(error) =
        persistence_service::save_client_state(&state.backend, disk_catalog.runtime_store())
    {
        let removed_runtime = disk_catalog.remove_runtime(&local_disk_id).ok_or_else(|| {
            ApiError::new("disk-not-found", "磁盘不存在", Some(local_disk_id.clone()))
        })?;
        disk_catalog.restore_pending_deletion(PendingDiskDeletion {
            deletion_id,
            removed_runtime,
        });

        return Err(error);
    }

    Ok(())
}

#[tauri::command]
pub fn commit_deleted_disk(
    state: State<'_, ClientState>,
    request: CommitDeletedDiskRequestDto,
) -> Result<(), ApiError> {
    runtime_rescan::ensure_runtime_rescan_idle(&state)?;

    let mut disk_catalog = state
        .disk_catalog
        .lock()
        .expect("disk catalog mutex should not be poisoned");

    disk_catalog
        .commit_pending_deletion(&request.deletion_id)
        .ok_or_else(|| {
            ApiError::new(
                "pending-deletion-not-found",
                "删除撤销窗口已结束",
                Some(request.deletion_id),
            )
        })?;

    Ok(())
}

#[tauri::command]
pub fn update_disk(
    state: State<'_, ClientState>,
    request: UpdateDiskRequestDto,
) -> Result<(), ApiError> {
    runtime_rescan::ensure_runtime_rescan_idle(&state)?;

    let mut disk_catalog = state
        .disk_catalog
        .lock()
        .expect("disk catalog mutex should not be poisoned");

    let updated_state = disk_service::update_disk(
        disk_catalog.runtime_store_mut(),
        disk_service::UpdateDiskRequest {
            local_disk_id: request.local_disk_id,
            disk_name: request.disk_name,
            auto_mount: request.auto_mount,
            configured_read_only: request.configured_read_only,
        },
    )?;

    if let Err(error) =
        persistence_service::save_client_state(&state.backend, disk_catalog.runtime_store())
    {
        let runtime = disk_catalog
            .runtime_store_mut()
            .find_runtime_mut(&updated_state.previous_snapshot.local_disk_id)
            .ok_or_else(|| {
                ApiError::new(
                    "disk-not-found",
                    "磁盘不存在",
                    Some(updated_state.previous_snapshot.local_disk_id.clone()),
                )
            })?;
        runtime.set_user_config(
            updated_state.previous_snapshot.disk_name,
            updated_state.previous_snapshot.auto_mount,
            updated_state.previous_snapshot.configured_read_only,
        );
        return Err(error);
    }

    Ok(())
}
