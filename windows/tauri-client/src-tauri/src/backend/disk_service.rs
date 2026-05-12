use std::collections::HashMap;

use backend_rust::BackendContext;
use backend_rust::ManagedDiskSnapshot;

use crate::state::disk_store::ConfigDiskRecord;
use crate::state::disk_store::DiskMediaConfig;
use crate::state::disk_store::DiskStore;

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

pub fn query_managed_disks(backend: &BackendContext) -> Vec<ManagedDiskSnapshot> {
    backend.snapshot_managed_disks()
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
