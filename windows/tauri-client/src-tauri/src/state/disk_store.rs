#![allow(dead_code)]

use std::collections::BTreeMap;

use backend_rust::Media;

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum MemoryMediaKind {
    DenseMem,
    SparseMem,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum FileMediaKind {
    RawFile,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub enum DiskMediaConfig {
    Memory {
        memory_kind: MemoryMediaKind,
        capacity_bytes: u64,
    },
    File {
        file_kind: FileMediaKind,
        file_path: String,
        capacity_bytes: u64,
    },
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct ConfigDiskRecord {
    pub disk_id: String,
    pub disk_name: String,
    pub auto_connect: bool,
    pub read_only: bool,
    pub media: DiskMediaConfig,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct ConnectedDiskRecord {
    pub disk_id: String,
    pub target_id: u32,
}

pub struct DiskStore {
    next_disk_number: u64,
    config_disks: Vec<ConfigDiskRecord>,
    connected_disks: Vec<ConnectedDiskRecord>,
    held_media_by_disk_id: BTreeMap<String, Box<dyn Media>>,
}

impl Default for DiskStore {
    fn default() -> Self {
        Self {
            next_disk_number: 1,
            config_disks: Vec::new(),
            connected_disks: Vec::new(),
            held_media_by_disk_id: BTreeMap::new(),
        }
    }
}

impl DiskStore {
    pub fn config_disks_snapshot(&self) -> Vec<ConfigDiskRecord> {
        self.config_disks.clone()
    }

    pub fn connected_disks_snapshot(&self) -> Vec<ConnectedDiskRecord> {
        self.connected_disks.clone()
    }

    pub fn allocate_disk_id(&mut self) -> String {
        let disk_id = format!("disk-{}", self.next_disk_number);
        self.next_disk_number += 1;
        disk_id
    }

    pub fn insert_unconnected_disk(
        &mut self,
        config_disk: ConfigDiskRecord,
        media: Box<dyn Media>,
    ) {
        let disk_id = config_disk.disk_id.clone();
        self.held_media_by_disk_id.insert(disk_id, media);
        self.config_disks.push(config_disk);
    }
}
