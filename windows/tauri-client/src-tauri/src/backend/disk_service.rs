use std::collections::HashMap;
use std::fs::OpenOptions;
use std::path::PathBuf;
use std::sync::Mutex;

use backend_rust::BackendContext;
use backend_rust::DiskConfig;
use backend_rust::ManagedDiskSnapshot;
use backend_rust::Media;
use rfd::FileDialog;

use crate::api_error::ApiError;
use crate::backend::memory_media::DenseMemoryMedia;
use crate::backend::memory_media::DenseMemoryMediaError;
use crate::backend::memory_media::SparseMemoryMedia;
use crate::backend::network_service;
use crate::backend::persistence_service;
use crate::state::disk_runtime::DiskMediaConfig;
use crate::state::disk_runtime::DiskRuntime;
use crate::state::disk_runtime::DiskRuntimeSnapshot;
use crate::state::disk_runtime::DiskRuntimeStatus;
use crate::state::disk_runtime::DiskRuntimeStore;
use crate::state::disk_runtime::FileMediaKind;
use crate::state::disk_runtime::MemoryMediaKind;
use crate::state::disk_runtime::RemovedDiskRuntime;
use crate::state::network_client::NetworkClientState;

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct HomeDiskListItemSnapshot {
    pub local_disk_id: String,
    pub disk_name: String,
    pub auto_mount: bool,
    pub read_only: bool,
    pub status: DiskRuntimeStatus,
    pub invalid_reason: Option<String>,
    pub online: bool,
    pub target_id: Option<u32>,
    pub lifecycle_text: String,
    pub media: DiskMediaConfig,
}

#[derive(Debug, Clone, Default, PartialEq, Eq)]
pub struct HomeDiskListSnapshot {
    pub disks: Vec<HomeDiskListItemSnapshot>,
}

pub struct UpdatedDiskState {
    pub previous_snapshot: DiskRuntimeSnapshot,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum RequestedMemoryMediaKind {
    Auto,
    DenseMem,
    SparseMem,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct CreateMemoryDiskRequest {
    pub disk_name: String,
    pub capacity_mib: u64,
    pub requested_memory_kind: RequestedMemoryMediaKind,
    pub auto_mount: bool,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct CreateFileDiskRequest {
    pub disk_name: String,
    pub file_path: String,
    pub auto_mount: bool,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum CreateFileFormat {
    Raw,
    Vmdk,
    Vhd,
    Vhdx,
    Vdi,
    Qcow2,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct CreateNewFileDiskRequest {
    pub disk_name: String,
    pub file_path: String,
    pub capacity_mib: u64,
    pub file_format: CreateFileFormat,
    pub auto_mount: bool,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct UpdateDiskRequest {
    pub local_disk_id: String,
    pub disk_name: String,
    pub auto_mount: bool,
}

const MIB_BYTES: u64 = 1024 * 1024;
const MAX_DENSE_MEMORY_BYTES: u64 = 1024 * 1024 * 1024;
const INVALID_FILE_REASON: &str = "文件路径必须指向已存在文件";

pub fn query_managed_disks(backend: &BackendContext) -> Vec<ManagedDiskSnapshot> {
    backend.snapshot_managed_disks()
}

pub fn query_home_disk_list(
    backend: &BackendContext,
    runtime_store: &DiskRuntimeStore,
) -> HomeDiskListSnapshot {
    let runtime_by_target = backend
        .snapshot_managed_disks()
        .into_iter()
        .map(|snapshot| (snapshot.target_id, snapshot))
        .collect::<HashMap<_, _>>();

    let disks = runtime_store
        .snapshots()
        .into_iter()
        .map(|runtime| map_home_disk_list_item_snapshot(runtime, &runtime_by_target))
        .collect();

    HomeDiskListSnapshot { disks }
}

pub fn create_memory_disk(
    runtime_store: &mut DiskRuntimeStore,
    request: CreateMemoryDiskRequest,
) -> Result<String, ApiError> {
    let disk_name = request.disk_name.trim();
    if disk_name.is_empty() {
        return Err(ApiError::new("invalid-disk-name", "磁盘名称不能为空", None));
    }

    if request.capacity_mib == 0 {
        return Err(ApiError::new(
            "invalid-disk-capacity",
            "容量必须是大于 0 的 MiB 整数",
            None,
        ));
    }

    let capacity_bytes = request.capacity_mib.checked_mul(MIB_BYTES).ok_or_else(|| {
        ApiError::new(
            "invalid-disk-capacity",
            "容量必须是大于 0 的 MiB 整数",
            None,
        )
    })?;

    let memory_kind = resolve_memory_kind(request.requested_memory_kind, capacity_bytes);
    let media = create_memory_media(memory_kind, capacity_bytes)?;
    let local_disk_id = runtime_store.allocate_local_disk_id();

    runtime_store.insert_runtime(DiskRuntime::new_memory(
        local_disk_id.clone(),
        disk_name.to_string(),
        request.auto_mount,
        memory_kind,
        capacity_bytes,
        media,
    ));

    Ok(local_disk_id)
}

pub fn pick_raw_file_path() -> Option<String> {
    FileDialog::new()
        .set_title("选择文件")
        .pick_file()
        .map(|path| path.display().to_string())
}

pub fn pick_new_raw_file_path() -> Option<String> {
    FileDialog::new()
        .set_title("选择新文件位置")
        .save_file()
        .map(|path| path.display().to_string())
}

pub fn create_file_disk(
    runtime_store: &mut DiskRuntimeStore,
    request: CreateFileDiskRequest,
) -> Result<String, ApiError> {
    let disk_name = request.disk_name.trim();
    if disk_name.is_empty() {
        return Err(ApiError::new("invalid-disk-name", "磁盘名称不能为空", None));
    }

    let file_path = request.file_path.trim();
    if file_path.is_empty() {
        return Err(ApiError::new("invalid-file-path", "文件路径不能为空", None));
    }

    let local_disk_id = runtime_store.allocate_local_disk_id();

    let runtime = match persistence_service::probe_raw_file_media(file_path) {
        Ok(probe) => DiskRuntime::new_file(
            local_disk_id.clone(),
            disk_name.to_string(),
            request.auto_mount,
            FileMediaKind::RawFile,
            file_path.to_string(),
            probe.capacity_bytes,
            probe.read_only,
            DiskRuntimeStatus::Unmounted,
            None,
        ),
        Err(_) => DiskRuntime::new_file(
            local_disk_id.clone(),
            disk_name.to_string(),
            request.auto_mount,
            FileMediaKind::RawFile,
            file_path.to_string(),
            0,
            false,
            DiskRuntimeStatus::Invalid {
                reason: INVALID_FILE_REASON.to_string(),
            },
            None,
        ),
    };

    runtime_store.insert_runtime(runtime);
    Ok(local_disk_id)
}

pub fn create_new_file_disk(
    runtime_store: &mut DiskRuntimeStore,
    request: CreateNewFileDiskRequest,
) -> Result<String, ApiError> {
    let disk_name = request.disk_name.trim();
    if disk_name.is_empty() {
        return Err(ApiError::new("invalid-disk-name", "磁盘名称不能为空", None));
    }

    let file_path = request.file_path.trim();
    if file_path.is_empty() {
        return Err(ApiError::new("invalid-file-path", "文件路径不能为空", None));
    }

    if request.capacity_mib == 0 {
        return Err(ApiError::new(
            "invalid-disk-capacity",
            "容量必须是大于 0 的 MiB 整数",
            None,
        ));
    }

    if request.file_format != CreateFileFormat::Raw {
        return Err(ApiError::new(
            "unsupported-file-format",
            "当前阶段只支持 RAW",
            None,
        ));
    }

    let capacity_bytes = request.capacity_mib.checked_mul(MIB_BYTES).ok_or_else(|| {
        ApiError::new(
            "invalid-disk-capacity",
            "容量必须是大于 0 的 MiB 整数",
            None,
        )
    })?;

    let file_path_buf = PathBuf::from(file_path);
    let parent_path = file_path_buf.parent().ok_or_else(|| {
        ApiError::new(
            "invalid-file-path",
            "文件路径必须包含父目录",
            Some(file_path.to_string()),
        )
    })?;

    if !parent_path.is_dir() {
        return Err(ApiError::new(
            "invalid-file-path",
            "目标目录不存在",
            Some(parent_path.display().to_string()),
        ));
    }

    if file_path_buf.exists() {
        return Err(ApiError::new(
            "file-already-exists",
            "目标文件已存在",
            Some(file_path.to_string()),
        ));
    }

    create_raw_file(&file_path_buf, capacity_bytes)?;

    match create_file_disk(
        runtime_store,
        CreateFileDiskRequest {
            disk_name: request.disk_name,
            file_path: request.file_path,
            auto_mount: request.auto_mount,
        },
    ) {
        Ok(local_disk_id) => Ok(local_disk_id),
        Err(error) => {
            let _ = std::fs::remove_file(&file_path_buf);
            Err(error)
        }
    }
}

pub fn mount_disk(
    backend: &BackendContext,
    runtime_store: &mut DiskRuntimeStore,
    network_client_mutex: &Mutex<NetworkClientState>,
    local_disk_id: &str,
) -> Result<u32, ApiError> {
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
        return network_service::mount_network_disk(
            backend,
            runtime_store,
            network_client_mutex,
            local_disk_id,
        );
    }

    let runtime = runtime_store
        .find_runtime_mut(local_disk_id)
        .ok_or_else(|| {
            ApiError::new(
                "disk-not-found",
                "磁盘不存在",
                Some(local_disk_id.to_string()),
            )
        })?;

    if let Some(target_id) = runtime.mounted_target_id() {
        return Err(ApiError::new(
            "disk-already-mounted",
            "磁盘已经处于挂载状态",
            Some(target_id.to_string()),
        ));
    }

    if runtime.invalid_reason().is_some() {
        return Err(ApiError::new(
            "disk-invalid",
            "磁盘当前无效，不能挂载",
            Some(INVALID_FILE_REASON.to_string()),
        ));
    }

    if runtime.media.is_none() {
        let file_path = runtime.file_path().ok_or_else(|| {
            ApiError::new(
                "held-media-not-found",
                "宿主未持有当前磁盘介质实例",
                Some(local_disk_id.to_string()),
            )
        })?;
        let media = persistence_service::open_raw_file_media(file_path)
            .map(|media| Box::new(media) as Box<dyn Media>)?;
        runtime.restore_media(media);
    }

    let disk_config = build_disk_config(runtime);
    let mut error_text = String::new();
    let media = runtime.take_media().ok_or_else(|| {
        ApiError::new(
            "held-media-not-found",
            "宿主未持有当前磁盘介质实例",
            Some(local_disk_id.to_string()),
        )
    })?;

    match backend.try_create_managed_disk(disk_config, media, Some(&mut error_text)) {
        Ok(target_id) => {
            runtime.set_mounted(target_id);
            Ok(target_id)
        }
        Err(media) => {
            runtime.restore_media(media);
            Err(ApiError::new(
                "mount-disk-failed",
                "挂载磁盘失败",
                Some(error_text),
            ))
        }
    }
}

pub fn eject_disk(
    backend: &BackendContext,
    runtime_store: &mut DiskRuntimeStore,
    local_disk_id: &str,
) -> Result<(), ApiError> {
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
        return network_service::eject_network_disk(backend, runtime_store, local_disk_id);
    }

    let runtime = runtime_store
        .find_runtime_mut(local_disk_id)
        .ok_or_else(|| {
            ApiError::new(
                "disk-not-found",
                "磁盘不存在",
                Some(local_disk_id.to_string()),
            )
        })?;

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

    if runtime.is_memory() {
        runtime.restore_media(media);
        runtime.set_unmounted();
        return Ok(());
    }

    drop(media);
    runtime.media = None;

    let file_path = runtime.file_path().ok_or_else(|| {
        ApiError::new(
            "disk-not-found",
            "磁盘不存在",
            Some(local_disk_id.to_string()),
        )
    })?;
    match persistence_service::probe_raw_file_media(file_path) {
        Ok(probe) => {
            runtime.set_file_unmounted(probe.capacity_bytes, probe.read_only);
        }
        Err(_) => {
            runtime.set_file_invalid(INVALID_FILE_REASON.to_string());
        }
    }

    Ok(())
}

pub fn prepare_deleted_runtime(
    backend: &BackendContext,
    removed_runtime: &mut RemovedDiskRuntime,
    _network_client_mutex: &Mutex<NetworkClientState>,
) -> Result<(), ApiError> {
    if removed_runtime.runtime.is_network() {
        return network_service::prepare_deleted_network_runtime(
            backend,
            removed_runtime,
            _network_client_mutex,
        );
    }

    if let Some(target_id) = removed_runtime.runtime.mounted_target_id() {
        let mut error_text = String::new();
        let media = backend
            .remove_managed_disk_with_media(target_id, Some(&mut error_text))
            .ok_or_else(|| ApiError::new("delete-disk-failed", "删除磁盘失败", Some(error_text)))?;

        if removed_runtime.runtime.is_memory() {
            removed_runtime.runtime.restore_media(media);
            removed_runtime.runtime.set_unmounted();
            return Ok(());
        }

        drop(media);
        removed_runtime.runtime.media = None;

        let file_path = removed_runtime.runtime.file_path().ok_or_else(|| {
            ApiError::new(
                "disk-not-found",
                "磁盘不存在",
                Some(removed_runtime.runtime.local_disk_id().to_string()),
            )
        })?;
        match persistence_service::probe_raw_file_media(file_path) {
            Ok(probe) => {
                removed_runtime
                    .runtime
                    .set_file_unmounted(probe.capacity_bytes, probe.read_only);
            }
            Err(_) => {
                removed_runtime
                    .runtime
                    .set_file_invalid(INVALID_FILE_REASON.to_string());
            }
        }
    }

    Ok(())
}

pub fn update_disk(
    runtime_store: &mut DiskRuntimeStore,
    request: UpdateDiskRequest,
) -> Result<UpdatedDiskState, ApiError> {
    let disk_name = request.disk_name.trim();
    if disk_name.is_empty() {
        return Err(ApiError::new("invalid-disk-name", "磁盘名称不能为空", None));
    }

    let runtime = runtime_store
        .find_runtime_mut(&request.local_disk_id)
        .ok_or_else(|| {
            ApiError::new(
                "disk-not-found",
                "磁盘不存在",
                Some(request.local_disk_id.to_string()),
            )
        })?;

    let previous_snapshot = runtime.snapshot();
    runtime.set_identity(disk_name.to_string(), request.auto_mount);

    Ok(UpdatedDiskState { previous_snapshot })
}

pub fn rescan_runtime_disks(
    backend: &BackendContext,
    runtime_store: &mut DiskRuntimeStore,
    network_client_mutex: &Mutex<NetworkClientState>,
) -> HomeDiskListSnapshot {
    for runtime in runtime_store.runtimes_mut() {
        if runtime.is_memory() {
            continue;
        }

        if runtime.is_network() {
            continue;
        }

        let Some(file_path) = runtime.file_path().map(str::to_string) else {
            continue;
        };

        if let Some(target_id) = runtime.mounted_target_id() {
            if persistence_service::probe_raw_file_media(&file_path).is_err() {
                let mut error_text = String::new();
                if let Some(media) =
                    backend.remove_managed_disk_with_media(target_id, Some(&mut error_text))
                {
                    drop(media);
                }

                runtime.set_file_invalid(INVALID_FILE_REASON.to_string());
            }

            continue;
        }

        match persistence_service::probe_raw_file_media(&file_path) {
            Ok(probe) => runtime.set_file_unmounted(probe.capacity_bytes, probe.read_only),
            Err(_) => runtime.set_file_invalid(INVALID_FILE_REASON.to_string()),
        }
    }

    network_service::rescan_network_runtimes(runtime_store, network_client_mutex);
    query_home_disk_list(backend, runtime_store)
}

fn resolve_memory_kind(
    requested_kind: RequestedMemoryMediaKind,
    capacity_bytes: u64,
) -> MemoryMediaKind {
    match requested_kind {
        RequestedMemoryMediaKind::DenseMem => MemoryMediaKind::DenseMem,
        RequestedMemoryMediaKind::SparseMem => MemoryMediaKind::SparseMem,
        RequestedMemoryMediaKind::Auto => {
            if capacity_bytes <= MAX_DENSE_MEMORY_BYTES {
                MemoryMediaKind::DenseMem
            } else {
                MemoryMediaKind::SparseMem
            }
        }
    }
}

fn create_memory_media(
    memory_kind: MemoryMediaKind,
    capacity_bytes: u64,
) -> Result<Box<dyn Media>, ApiError> {
    match memory_kind {
        MemoryMediaKind::DenseMem => DenseMemoryMedia::new(capacity_bytes)
            .map(|media| Box::new(media) as Box<dyn Media>)
            .map_err(|error| map_dense_memory_media_error(error, capacity_bytes)),
        MemoryMediaKind::SparseMem => Ok(Box::new(SparseMemoryMedia::new(capacity_bytes))),
    }
}

fn map_dense_memory_media_error(error: DenseMemoryMediaError, capacity_bytes: u64) -> ApiError {
    match error {
        DenseMemoryMediaError::SizeExceedsProcessLimit => ApiError::new(
            "dense-memory-size-exceeds-process-limit",
            "denseMem 大小超出当前进程可分配范围",
            Some(capacity_bytes.to_string()),
        ),
        DenseMemoryMediaError::AllocationFailed => ApiError::new(
            "dense-memory-allocation-failed",
            "denseMem 分配失败",
            Some(capacity_bytes.to_string()),
        ),
    }
}

fn create_raw_file(file_path: &PathBuf, capacity_bytes: u64) -> Result<(), ApiError> {
    let file = OpenOptions::new()
        .write(true)
        .create_new(true)
        .open(file_path)
        .map_err(|io_error| {
            ApiError::new(
                "create-file-open-failed",
                "创建目标文件失败",
                Some(format!("{} | {}", file_path.display(), io_error)),
            )
        })?;

    file.set_len(capacity_bytes).map_err(|io_error| {
        ApiError::new(
            "create-file-size-failed",
            "设置目标文件大小失败",
            Some(format!("{} | {}", file_path.display(), io_error)),
        )
    })
}

fn map_home_disk_list_item_snapshot(
    runtime: DiskRuntimeSnapshot,
    runtime_by_target: &HashMap<u32, ManagedDiskSnapshot>,
) -> HomeDiskListItemSnapshot {
    let target_id = match runtime.status {
        DiskRuntimeStatus::Mounted { target_id } => Some(target_id),
        DiskRuntimeStatus::Unmounted | DiskRuntimeStatus::Invalid { .. } => None,
    };
    let runtime_snapshot = target_id.and_then(|value| runtime_by_target.get(&value));

    HomeDiskListItemSnapshot {
        local_disk_id: runtime.local_disk_id,
        disk_name: runtime.disk_name,
        auto_mount: runtime.auto_mount,
        read_only: runtime.read_only,
        invalid_reason: match &runtime.status {
            DiskRuntimeStatus::Invalid { reason } => Some(reason.clone()),
            DiskRuntimeStatus::Unmounted | DiskRuntimeStatus::Mounted { .. } => None,
        },
        status: runtime.status,
        online: runtime_snapshot
            .map(|snapshot| snapshot.online)
            .unwrap_or(false),
        target_id,
        lifecycle_text: runtime_snapshot
            .map(|snapshot| snapshot.lifecycle_text.clone())
            .unwrap_or_default(),
        media: runtime.media,
    }
}

fn build_disk_config(runtime: &DiskRuntime) -> DiskConfig {
    DiskConfig {
        disk_size_bytes: runtime.capacity_bytes(),
        read_only: runtime.read_only(),
        ..DiskConfig::default()
    }
}
