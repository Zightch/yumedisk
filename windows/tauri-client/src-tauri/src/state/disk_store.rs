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
    pub valid: bool,
    pub invalid_reason: Option<String>,
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

    pub fn insert_disk_record(&mut self, config_disk: ConfigDiskRecord, media: Option<Box<dyn Media>>) {
        let disk_id = config_disk.disk_id.clone();
        self.bump_next_disk_number_from_disk_id(&disk_id);
        if let Some(media) = media {
            self.held_media_by_disk_id.insert(disk_id, media);
        }
        self.config_disks.push(config_disk);
    }

    pub fn insert_unconnected_disk(&mut self, config_disk: ConfigDiskRecord, media: Box<dyn Media>) {
        self.insert_disk_record(config_disk, Some(media));
    }

    pub fn find_config_disk(&self, disk_id: &str) -> Option<ConfigDiskRecord> {
        self.config_disks
            .iter()
            .find(|disk| disk.disk_id == disk_id)
            .cloned()
    }

    pub fn take_held_media(&mut self, disk_id: &str) -> Option<Box<dyn Media>> {
        self.held_media_by_disk_id.remove(disk_id)
    }

    pub fn put_held_media(&mut self, disk_id: String, media: Box<dyn Media>) {
        self.held_media_by_disk_id.insert(disk_id, media);
    }

    pub fn insert_connected_disk(&mut self, disk_id: String, target_id: u32) {
        self.connected_disks.retain(|disk| disk.disk_id != disk_id);
        self.connected_disks
            .push(ConnectedDiskRecord { disk_id, target_id });
    }

    pub fn remove_connected_disk(&mut self, disk_id: &str) -> Option<ConnectedDiskRecord> {
        let index = self
            .connected_disks
            .iter()
            .position(|disk| disk.disk_id == disk_id)?;
        Some(self.connected_disks.remove(index))
    }

    pub fn find_connected_disk(&self, disk_id: &str) -> Option<ConnectedDiskRecord> {
        self.connected_disks
            .iter()
            .find(|disk| disk.disk_id == disk_id)
            .cloned()
    }

    pub fn remove_config_disk(&mut self, disk_id: &str) -> Option<ConfigDiskRecord> {
        let index = self
            .config_disks
            .iter()
            .position(|disk| disk.disk_id == disk_id)?;
        Some(self.config_disks.remove(index))
    }

    pub fn restore_disk_record(
        &mut self,
        config_disk: ConfigDiskRecord,
        connected_record: Option<ConnectedDiskRecord>,
        media: Option<Box<dyn Media>>,
    ) {
        let disk_id = config_disk.disk_id.clone();
        self.insert_disk_record(config_disk, media);
        if let Some(connected_record) = connected_record {
            self.insert_connected_disk(disk_id, connected_record.target_id);
        }
    }

    pub fn remove_unconnected_disk(&mut self, disk_id: &str) -> bool {
        let previous_count = self.config_disks.len();
        self.config_disks.retain(|disk| disk.disk_id != disk_id);
        self.connected_disks.retain(|disk| disk.disk_id != disk_id);
        self.held_media_by_disk_id.remove(disk_id);
        previous_count != self.config_disks.len()
    }

    fn bump_next_disk_number_from_disk_id(&mut self, disk_id: &str) {
        let Some(number_text) = disk_id.strip_prefix("disk-") else {
            return;
        };

        let Ok(number) = number_text.parse::<u64>() else {
            return;
        };

        if number >= self.next_disk_number {
            self.next_disk_number = number + 1;
        }
    }
}
