#![allow(dead_code)]

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

#[derive(Debug, Default)]
pub struct DiskStore {
    config_disks: Vec<ConfigDiskRecord>,
    connected_disks: Vec<ConnectedDiskRecord>,
}

impl DiskStore {
    pub fn config_disks_snapshot(&self) -> Vec<ConfigDiskRecord> {
        self.config_disks.clone()
    }

    pub fn connected_disks_snapshot(&self) -> Vec<ConnectedDiskRecord> {
        self.connected_disks.clone()
    }
}
