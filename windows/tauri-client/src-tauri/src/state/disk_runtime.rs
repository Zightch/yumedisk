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
pub enum DiskRuntimeStatus {
    Disconnected,
    Connected { target_id: u32 },
    Invalid { reason: String },
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
pub struct DiskRuntimeSnapshot {
    pub disk_id: String,
    pub disk_name: String,
    pub auto_connect: bool,
    pub read_only: bool,
    pub status: DiskRuntimeStatus,
    pub media: DiskMediaConfig,
}

pub struct RemovedDiskRuntime {
    pub(crate) index: usize,
    pub(crate) runtime: DiskRuntime,
}

pub struct DiskRuntimeStore {
    next_disk_number: u64,
    runtimes: Vec<DiskRuntime>,
}

pub enum DiskRuntime {
    Memory(MemoryDiskRuntime),
    File(FileDiskRuntime),
}

pub struct MemoryDiskRuntime {
    pub(crate) disk_id: String,
    pub(crate) disk_name: String,
    pub(crate) auto_connect: bool,
    pub(crate) memory_kind: MemoryMediaKind,
    pub(crate) capacity_bytes: u64,
    pub(crate) state: DiskRuntimeStatus,
    pub(crate) held_media: Option<Box<dyn Media>>,
}

pub struct FileDiskRuntime {
    pub(crate) disk_id: String,
    pub(crate) disk_name: String,
    pub(crate) auto_connect: bool,
    pub(crate) file_kind: FileMediaKind,
    pub(crate) file_path: String,
    pub(crate) capacity_bytes: u64,
    pub(crate) read_only: bool,
    pub(crate) state: DiskRuntimeStatus,
}

impl Default for DiskRuntimeStore {
    fn default() -> Self {
        Self {
            next_disk_number: 1,
            runtimes: Vec::new(),
        }
    }
}

impl DiskRuntimeStore {
    pub fn allocate_disk_id(&mut self) -> String {
        let disk_id = format!("disk-{}", self.next_disk_number);
        self.next_disk_number += 1;
        disk_id
    }

    pub fn insert_runtime(&mut self, runtime: DiskRuntime) {
        self.bump_next_disk_number_from_disk_id(runtime.disk_id());
        self.runtimes.push(runtime);
    }

    pub fn restore_removed_runtime(&mut self, removed: RemovedDiskRuntime) {
        self.bump_next_disk_number_from_disk_id(removed.runtime.disk_id());
        let index = removed.index.min(self.runtimes.len());
        self.runtimes.insert(index, removed.runtime);
    }

    pub fn snapshots(&self) -> Vec<DiskRuntimeSnapshot> {
        self.runtimes.iter().map(DiskRuntime::snapshot).collect()
    }

    pub fn find_runtime_mut(&mut self, disk_id: &str) -> Option<&mut DiskRuntime> {
        self.runtimes
            .iter_mut()
            .find(|runtime| runtime.disk_id() == disk_id)
    }

    pub fn runtimes_mut(&mut self) -> std::slice::IterMut<'_, DiskRuntime> {
        self.runtimes.iter_mut()
    }

    pub fn remove_runtime(&mut self, disk_id: &str) -> Option<RemovedDiskRuntime> {
        let index = self
            .runtimes
            .iter()
            .position(|runtime| runtime.disk_id() == disk_id)?;
        Some(RemovedDiskRuntime {
            index,
            runtime: self.runtimes.remove(index),
        })
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

impl DiskRuntime {
    pub fn new_memory(
        disk_id: String,
        disk_name: String,
        auto_connect: bool,
        memory_kind: MemoryMediaKind,
        capacity_bytes: u64,
        media: Box<dyn Media>,
    ) -> Self {
        Self::Memory(MemoryDiskRuntime {
            disk_id,
            disk_name,
            auto_connect,
            memory_kind,
            capacity_bytes,
            state: DiskRuntimeStatus::Disconnected,
            held_media: Some(media),
        })
    }

    pub fn new_file_disconnected(
        disk_id: String,
        disk_name: String,
        auto_connect: bool,
        file_kind: FileMediaKind,
        file_path: String,
        capacity_bytes: u64,
        read_only: bool,
    ) -> Self {
        Self::File(FileDiskRuntime {
            disk_id,
            disk_name,
            auto_connect,
            file_kind,
            file_path,
            capacity_bytes,
            read_only,
            state: DiskRuntimeStatus::Disconnected,
        })
    }

    pub fn new_file_invalid(
        disk_id: String,
        disk_name: String,
        auto_connect: bool,
        file_kind: FileMediaKind,
        file_path: String,
        reason: String,
    ) -> Self {
        Self::File(FileDiskRuntime {
            disk_id,
            disk_name,
            auto_connect,
            file_kind,
            file_path,
            capacity_bytes: 0,
            read_only: false,
            state: DiskRuntimeStatus::Invalid { reason },
        })
    }

    pub fn disk_id(&self) -> &str {
        match self {
            Self::Memory(runtime) => &runtime.disk_id,
            Self::File(runtime) => &runtime.disk_id,
        }
    }

    pub fn disk_name(&self) -> &str {
        match self {
            Self::Memory(runtime) => &runtime.disk_name,
            Self::File(runtime) => &runtime.disk_name,
        }
    }

    pub fn auto_connect(&self) -> bool {
        match self {
            Self::Memory(runtime) => runtime.auto_connect,
            Self::File(runtime) => runtime.auto_connect,
        }
    }

    pub fn set_identity(&mut self, disk_name: String, auto_connect: bool) {
        match self {
            Self::Memory(runtime) => {
                runtime.disk_name = disk_name;
                runtime.auto_connect = auto_connect;
            }
            Self::File(runtime) => {
                runtime.disk_name = disk_name;
                runtime.auto_connect = auto_connect;
            }
        }
    }

    pub fn read_only(&self) -> bool {
        match self {
            Self::Memory(_) => false,
            Self::File(runtime) => runtime.read_only,
        }
    }

    pub fn capacity_bytes(&self) -> u64 {
        match self {
            Self::Memory(runtime) => runtime.capacity_bytes,
            Self::File(runtime) => runtime.capacity_bytes,
        }
    }

    pub fn status(&self) -> &DiskRuntimeStatus {
        match self {
            Self::Memory(runtime) => &runtime.state,
            Self::File(runtime) => &runtime.state,
        }
    }

    pub fn connected_target_id(&self) -> Option<u32> {
        match self.status() {
            DiskRuntimeStatus::Connected { target_id } => Some(*target_id),
            DiskRuntimeStatus::Disconnected | DiskRuntimeStatus::Invalid { .. } => None,
        }
    }

    pub fn invalid_reason(&self) -> Option<&str> {
        match self.status() {
            DiskRuntimeStatus::Invalid { reason } => Some(reason.as_str()),
            DiskRuntimeStatus::Disconnected | DiskRuntimeStatus::Connected { .. } => None,
        }
    }

    pub fn is_memory(&self) -> bool {
        matches!(self, Self::Memory(_))
    }

    pub fn file_path(&self) -> Option<&str> {
        match self {
            Self::Memory(_) => None,
            Self::File(runtime) => Some(runtime.file_path.as_str()),
        }
    }

    pub fn set_file_disconnected(&mut self, capacity_bytes: u64, read_only: bool) {
        if let Self::File(runtime) = self {
            runtime.capacity_bytes = capacity_bytes;
            runtime.read_only = read_only;
            runtime.state = DiskRuntimeStatus::Disconnected;
        }
    }

    pub fn set_file_invalid(&mut self, reason: String) {
        if let Self::File(runtime) = self {
            runtime.capacity_bytes = 0;
            runtime.read_only = false;
            runtime.state = DiskRuntimeStatus::Invalid { reason };
        }
    }

    pub fn media_snapshot(&self) -> DiskMediaConfig {
        match self {
            Self::Memory(runtime) => DiskMediaConfig::Memory {
                memory_kind: runtime.memory_kind,
                capacity_bytes: runtime.capacity_bytes,
            },
            Self::File(runtime) => DiskMediaConfig::File {
                file_kind: runtime.file_kind,
                file_path: runtime.file_path.clone(),
                capacity_bytes: runtime.capacity_bytes,
            },
        }
    }

    pub fn snapshot(&self) -> DiskRuntimeSnapshot {
        DiskRuntimeSnapshot {
            disk_id: self.disk_id().to_string(),
            disk_name: self.disk_name().to_string(),
            auto_connect: self.auto_connect(),
            read_only: self.read_only(),
            status: self.status().clone(),
            media: self.media_snapshot(),
        }
    }
}
