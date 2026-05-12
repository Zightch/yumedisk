use std::collections::HashMap;

use backend_rust::BackendContext;
use backend_rust::Media;
use backend_rust::ManagedDiskSnapshot;

use crate::api_error::ApiError;
use crate::backend::memory_media::DenseMemoryMedia;
use crate::backend::memory_media::DenseMemoryMediaError;
use crate::backend::memory_media::SparseMemoryMedia;
use crate::state::disk_store::ConfigDiskRecord;
use crate::state::disk_store::DiskMediaConfig;
use crate::state::disk_store::DiskStore;
use crate::state::disk_store::MemoryMediaKind;

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct HomeDiskListItemSnapshot {
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
    pub media: DiskMediaConfig,
}

#[derive(Debug, Clone, Default, PartialEq, Eq)]
pub struct HomeDiskListSnapshot {
    pub disks: Vec<HomeDiskListItemSnapshot>,
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

const MIB_BYTES: u64 = 1024 * 1024;
const MAX_DENSE_MEMORY_BYTES: u64 = 1024 * 1024 * 1024;

pub fn query_managed_disks(backend: &BackendContext) -> Vec<ManagedDiskSnapshot> {
    backend.snapshot_managed_disks()
}

pub fn create_memory_disk(
    disk_store: &mut DiskStore,
    request: CreateMemoryDiskRequest,
) -> Result<String, ApiError> {
    let disk_name = request.disk_name.trim();
    if disk_name.is_empty() {
        return Err(ApiError::new(
            "invalid-disk-name",
            "磁盘名称不能为空",
            None,
        ));
    }

    if request.capacity_mib == 0 {
        return Err(ApiError::new(
            "invalid-disk-capacity",
            "容量必须是大于 0 的 MiB 整数",
            None,
        ));
    }

    let capacity_bytes = request
        .capacity_mib
        .checked_mul(MIB_BYTES)
        .ok_or_else(|| {
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
            media: DiskMediaConfig::Memory {
                memory_kind,
                capacity_bytes,
            },
        },
        media,
    );

    Ok(disk_id)
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

            map_home_disk_list_item_snapshot(
                config_disk,
                connected_target_id,
                &runtime_by_target,
            )
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

fn map_dense_memory_media_error(
    error: DenseMemoryMediaError,
    capacity_bytes: u64,
) -> ApiError {
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

fn map_home_disk_list_item_snapshot(
    config_disk: ConfigDiskRecord,
    connected_target_id: Option<u32>,
    runtime_by_target: &HashMap<u32, ManagedDiskSnapshot>,
) -> HomeDiskListItemSnapshot {
    let runtime_snapshot = connected_target_id.and_then(|value| runtime_by_target.get(&value));

    HomeDiskListItemSnapshot {
        disk_id: config_disk.disk_id,
        disk_name: config_disk.disk_name,
        auto_connect: config_disk.auto_connect,
        read_only: config_disk.read_only,
        connected: connected_target_id.is_some(),
        online: runtime_snapshot.map(|snapshot| snapshot.online).unwrap_or(false),
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
