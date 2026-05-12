use std::collections::HashMap;
use std::fs::OpenOptions;
use std::path::PathBuf;

use backend_rust::BackendContext;
use backend_rust::DiskConfig;
use backend_rust::ManagedDiskSnapshot;
use backend_rust::Media;
use rfd::FileDialog;

use crate::api_error::ApiError;
use crate::backend::memory_media::DenseMemoryMedia;
use crate::backend::memory_media::DenseMemoryMediaError;
use crate::backend::memory_media::SparseMemoryMedia;
use crate::backend::raw_file_media::RawFileMedia;
use crate::backend::raw_file_media::RawFileMediaError;
use crate::state::disk_store::ConfigDiskRecord;
use crate::state::disk_store::DiskMediaConfig;
use crate::state::disk_store::DiskStatus;
use crate::state::disk_store::DiskStore;
use crate::state::disk_store::FileMediaKind;
use crate::state::disk_store::MemoryMediaKind;

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct HomeDiskListItemSnapshot {
    pub disk_id: String,
    pub disk_name: String,
    pub auto_connect: bool,
    pub read_only: bool,
    pub status: DiskStatus,
    pub invalid_reason: Option<String>,
    pub online: bool,
    pub target_id: Option<u32>,
    pub lifecycle_text: String,
    pub visible_path: String,
    pub physical_drive_path: String,
    pub media: DiskMediaConfig,
}

#[derive(Debug, Clone, Default, PartialEq, Eq)]
pub struct HomeDiskListSnapshot {
    pub disks: Vec<HomeDiskListItemSnapshot>,
}

pub struct DeletedDiskState {
    pub config_disk: ConfigDiskRecord,
    pub media: Option<Box<dyn Media>>,
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
    pub auto_connect: bool,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct CreateFileDiskRequest {
    pub disk_name: String,
    pub file_path: String,
    pub auto_connect: bool,
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
    pub auto_connect: bool,
}

const MIB_BYTES: u64 = 1024 * 1024;
const MAX_DENSE_MEMORY_BYTES: u64 = 1024 * 1024 * 1024;

pub fn query_managed_disks(backend: &BackendContext) -> Vec<ManagedDiskSnapshot> {
    backend.snapshot_managed_disks()
}

pub fn connect_disk(
    backend: &BackendContext,
    disk_store: &mut DiskStore,
    disk_id: &str,
) -> Result<u32, ApiError> {
    let config_disk = disk_store
        .find_config_disk(disk_id)
        .ok_or_else(|| ApiError::new("disk-not-found", "磁盘不存在", Some(disk_id.to_string())))?;

    if disk_store.find_connected_disk(disk_id).is_some() {
        return Err(ApiError::new(
            "disk-already-connected",
            "磁盘已经处于连接状态",
            Some(disk_id.to_string()),
        ));
    }

    if matches!(resolve_disk_status(&config_disk, None), DiskStatus::Invalid) {
        return Err(ApiError::new(
            "disk-invalid",
            "磁盘当前无效，不能连接",
            config_disk.invalid_reason.clone(),
        ));
    }

    let media = disk_store.take_held_media(disk_id).ok_or_else(|| {
        ApiError::new(
            "held-media-not-found",
            "宿主未持有当前磁盘介质实例",
            Some(disk_id.to_string()),
        )
    })?;

    let disk_config = build_disk_config(&config_disk);
    let mut error_text = String::new();
    match backend.try_create_managed_disk(disk_config, media, Some(&mut error_text)) {
        Ok(target_id) => {
            disk_store.insert_connected_disk(disk_id.to_string(), target_id);
            Ok(target_id)
        }
        Err(media) => {
            disk_store.put_held_media(disk_id.to_string(), media);
            Err(ApiError::new(
                "connect-disk-failed",
                "连接磁盘失败",
                Some(error_text),
            ))
        }
    }
}

pub fn disconnect_disk(
    backend: &BackendContext,
    disk_store: &mut DiskStore,
    disk_id: &str,
) -> Result<(), ApiError> {
    let connected_record = disk_store.find_connected_disk(disk_id).ok_or_else(|| {
        ApiError::new(
            "disk-not-connected",
            "磁盘当前未连接",
            Some(disk_id.to_string()),
        )
    })?;

    let mut error_text = String::new();
    let media = backend
        .remove_managed_disk_with_media(connected_record.target_id, Some(&mut error_text))
        .ok_or_else(|| ApiError::new("disconnect-disk-failed", "断开磁盘失败", Some(error_text)))?;

    let _ = disk_store.remove_connected_disk(disk_id);
    disk_store.put_held_media(disk_id.to_string(), media);
    Ok(())
}

pub fn delete_disk(
    backend: &BackendContext,
    disk_store: &mut DiskStore,
    disk_id: &str,
) -> Result<DeletedDiskState, ApiError> {
    let config_disk = disk_store.find_config_disk(disk_id).ok_or_else(|| {
        ApiError::new("disk-not-found", "磁盘不存在", Some(disk_id.to_string()))
    })?;
    let connected_record = disk_store.find_connected_disk(disk_id);
    let mut reclaimed_media = None;

    if let Some(connected_record) = connected_record.clone() {
        let mut error_text = String::new();
        let media = backend
            .remove_managed_disk_with_media(connected_record.target_id, Some(&mut error_text))
            .ok_or_else(|| {
                ApiError::new("delete-disk-failed", "删除磁盘失败", Some(error_text))
            })?;
        let _ = disk_store.remove_connected_disk(disk_id);
        reclaimed_media = Some(media);
    }

    let media = reclaimed_media.or_else(|| disk_store.take_held_media(disk_id));
    let _ = disk_store.remove_config_disk(disk_id);
    Ok(DeletedDiskState { config_disk, media })
}

pub fn create_memory_disk(
    disk_store: &mut DiskStore,
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
    let disk_id = disk_store.allocate_disk_id();

    disk_store.insert_unconnected_disk(
        ConfigDiskRecord {
            disk_id: disk_id.clone(),
            disk_name: disk_name.to_string(),
            auto_connect: request.auto_connect,
            read_only: false,
            invalid_reason: None,
            media: DiskMediaConfig::Memory {
                memory_kind,
                capacity_bytes,
            },
        },
        media,
    );

    Ok(disk_id)
}

pub fn pick_raw_file_path() -> Option<String> {
    FileDialog::new()
        .set_title("选择 RAW 文件")
        .pick_file()
        .map(|path| path.display().to_string())
}

pub fn pick_new_raw_file_path() -> Option<String> {
    FileDialog::new()
        .set_title("选择新 RAW 文件位置")
        .add_filter("RAW", &["raw"])
        .save_file()
        .map(|path| path.display().to_string())
}

pub fn create_file_disk(
    disk_store: &mut DiskStore,
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

    let file_path_buf = PathBuf::from(file_path);
    if !file_path_buf.is_file() {
        return Err(ApiError::new(
            "invalid-file-path",
            "文件路径必须指向已存在文件",
            Some(file_path.to_string()),
        ));
    }

    let media = RawFileMedia::open(&file_path_buf)
        .map_err(|error| map_raw_file_media_error(error, file_path))?;
    let capacity_bytes = media.size_bytes();
    let read_only = media.read_only();
    let disk_id = disk_store.allocate_disk_id();

    disk_store.insert_unconnected_disk(
        ConfigDiskRecord {
            disk_id: disk_id.clone(),
            disk_name: disk_name.to_string(),
            auto_connect: request.auto_connect,
            read_only,
            invalid_reason: None,
            media: DiskMediaConfig::File {
                file_kind: FileMediaKind::RawFile,
                file_path: file_path.to_string(),
                capacity_bytes,
            },
        },
        Box::new(media),
    );

    Ok(disk_id)
}

pub fn create_new_file_disk(
    disk_store: &mut DiskStore,
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
        disk_store,
        CreateFileDiskRequest {
            disk_name: request.disk_name,
            file_path: request.file_path,
            auto_connect: request.auto_connect,
        },
    ) {
        Ok(disk_id) => Ok(disk_id),
        Err(error) => {
            let _ = std::fs::remove_file(&file_path_buf);
            Err(error)
        }
    }
}

pub fn query_home_disk_list(
    backend: &BackendContext,
    disk_store: &DiskStore,
) -> HomeDiskListSnapshot {
    let runtime_by_target = backend
        .snapshot_managed_disks()
        .into_iter()
        .map(|snapshot| (snapshot.target_id, snapshot))
        .collect::<HashMap<_, _>>();

    let connected_by_disk_id = disk_store
        .connected_disks_snapshot()
        .into_iter()
        .map(|record| (record.disk_id, record.target_id))
        .collect::<HashMap<_, _>>();

    let disks = disk_store
        .config_disks_snapshot()
        .into_iter()
        .map(|config_disk| {
            let connected_target_id = connected_by_disk_id.get(&config_disk.disk_id).copied();

            map_home_disk_list_item_snapshot(config_disk, connected_target_id, &runtime_by_target)
        })
        .collect();

    HomeDiskListSnapshot { disks }
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

fn map_raw_file_media_error(error: RawFileMediaError, file_path: &str) -> ApiError {
    match error {
        RawFileMediaError::OpenFailed {
            read_write,
            read_only,
        } => ApiError::new(
            "raw-file-open-failed",
            "无法打开 RAW 文件",
            Some(format!(
                "{} | readWrite={} | readOnly={}",
                file_path, read_write, read_only
            )),
        ),
        RawFileMediaError::MetadataFailed(io_error) => ApiError::new(
            "raw-file-metadata-failed",
            "读取 RAW 文件信息失败",
            Some(format!("{} | {}", file_path, io_error)),
        ),
        RawFileMediaError::EmptyFile => ApiError::new(
            "raw-file-empty",
            "RAW 文件大小必须大于 0",
            Some(file_path.to_string()),
        ),
    }
}

fn map_home_disk_list_item_snapshot(
    config_disk: ConfigDiskRecord,
    connected_target_id: Option<u32>,
    runtime_by_target: &HashMap<u32, ManagedDiskSnapshot>,
) -> HomeDiskListItemSnapshot {
    let status = resolve_disk_status(&config_disk, connected_target_id);
    let runtime_snapshot = connected_target_id.and_then(|value| runtime_by_target.get(&value));

    HomeDiskListItemSnapshot {
        disk_id: config_disk.disk_id,
        disk_name: config_disk.disk_name,
        auto_connect: config_disk.auto_connect,
        read_only: config_disk.read_only,
        status,
        invalid_reason: config_disk.invalid_reason,
        online: runtime_snapshot
            .map(|snapshot| snapshot.online)
            .unwrap_or(false),
        target_id: connected_target_id,
        lifecycle_text: runtime_snapshot
            .map(|snapshot| snapshot.lifecycle_text.clone())
            .unwrap_or_default(),
        visible_path: runtime_snapshot
            .map(|snapshot| snapshot.visible_path.clone())
            .unwrap_or_default(),
        physical_drive_path: runtime_snapshot
            .map(|snapshot| snapshot.physical_drive_path.clone())
            .unwrap_or_default(),
        media: config_disk.media,
    }
}

fn resolve_disk_status(config_disk: &ConfigDiskRecord, connected_target_id: Option<u32>) -> DiskStatus {
    if config_disk.invalid_reason.is_some() {
        return DiskStatus::Invalid;
    }

    if connected_target_id.is_some() {
        return DiskStatus::Connected;
    }

    DiskStatus::Disconnected
}

fn build_disk_config(config_disk: &ConfigDiskRecord) -> DiskConfig {
    DiskConfig {
        disk_size_bytes: match &config_disk.media {
            DiskMediaConfig::Memory { capacity_bytes, .. } => *capacity_bytes,
            DiskMediaConfig::File { capacity_bytes, .. } => *capacity_bytes,
        },
        read_only: config_disk.read_only,
        ..DiskConfig::default()
    }
}
