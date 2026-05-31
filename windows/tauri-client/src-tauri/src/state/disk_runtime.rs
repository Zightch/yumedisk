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
    Unmounted,
    Mounted { target_id: u32 },
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
    Network {
        server_addr: String,
        remote_disk_id: String,
        auth_material: String,
        capacity_bytes: u64,
    },
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct DiskRuntimeSnapshot {
    pub local_disk_id: String,
    pub disk_name: String,
    pub auto_mount: bool,
    pub configured_read_only: bool,
    pub source_read_only: bool,
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

pub struct DiskRuntime {
    pub(crate) local_disk_id: String,
    pub(crate) disk_name: String,
    pub(crate) auto_mount: bool,
    pub(crate) configured_read_only: bool,
    pub(crate) source_read_only: bool,
    pub(crate) state: DiskRuntimeStatus,
    pub(crate) media_config: DiskMediaConfig,
    pub(crate) media: Option<Box<dyn Media>>,
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
    pub fn allocate_local_disk_id(&mut self) -> String {
        let local_disk_id = format!("disk-{}", self.next_disk_number);
        self.next_disk_number += 1;
        local_disk_id
    }

    pub fn insert_runtime(&mut self, runtime: DiskRuntime) {
        self.bump_next_disk_number_from_local_disk_id(runtime.local_disk_id());
        self.runtimes.push(runtime);
    }

    pub fn restore_removed_runtime(&mut self, removed: RemovedDiskRuntime) {
        self.bump_next_disk_number_from_local_disk_id(removed.runtime.local_disk_id());
        let index = removed.index.min(self.runtimes.len());
        self.runtimes.insert(index, removed.runtime);
    }

    pub fn snapshots(&self) -> Vec<DiskRuntimeSnapshot> {
        self.runtimes.iter().map(DiskRuntime::snapshot).collect()
    }

    pub fn find_runtime_mut(&mut self, local_disk_id: &str) -> Option<&mut DiskRuntime> {
        self.runtimes
            .iter_mut()
            .find(|runtime| runtime.local_disk_id() == local_disk_id)
    }

    pub fn find_runtime(&self, local_disk_id: &str) -> Option<&DiskRuntime> {
        self.runtimes
            .iter()
            .find(|runtime| runtime.local_disk_id() == local_disk_id)
    }

    pub fn find_runtime_mut_by_target_id(&mut self, target_id: u32) -> Option<&mut DiskRuntime> {
        self.runtimes
            .iter_mut()
            .find(|runtime| runtime.mounted_target_id() == Some(target_id))
    }

    pub fn runtimes_mut(&mut self) -> std::slice::IterMut<'_, DiskRuntime> {
        self.runtimes.iter_mut()
    }

    pub fn runtimes(&self) -> std::slice::Iter<'_, DiskRuntime> {
        self.runtimes.iter()
    }

    pub fn remove_runtime(&mut self, local_disk_id: &str) -> Option<RemovedDiskRuntime> {
        let index = self
            .runtimes
            .iter()
            .position(|runtime| runtime.local_disk_id() == local_disk_id)?;
        Some(RemovedDiskRuntime {
            index,
            runtime: self.runtimes.remove(index),
        })
    }

    fn bump_next_disk_number_from_local_disk_id(&mut self, local_disk_id: &str) {
        let Some(number_text) = local_disk_id.strip_prefix("disk-") else {
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
        local_disk_id: String,
        disk_name: String,
        auto_mount: bool,
        configured_read_only: bool,
        memory_kind: MemoryMediaKind,
        capacity_bytes: u64,
        media: Box<dyn Media>,
    ) -> Self {
        Self {
            local_disk_id,
            disk_name,
            auto_mount,
            configured_read_only,
            source_read_only: false,
            state: DiskRuntimeStatus::Unmounted,
            media_config: DiskMediaConfig::Memory {
                memory_kind,
                capacity_bytes,
            },
            media: Some(media),
        }
    }

    pub fn new_file(
        local_disk_id: String,
        disk_name: String,
        auto_mount: bool,
        configured_read_only: bool,
        file_kind: FileMediaKind,
        file_path: String,
        capacity_bytes: u64,
        source_read_only: bool,
        status: DiskRuntimeStatus,
        media: Option<Box<dyn Media>>,
    ) -> Self {
        Self {
            local_disk_id,
            disk_name,
            auto_mount,
            configured_read_only,
            source_read_only,
            state: status,
            media_config: DiskMediaConfig::File {
                file_kind,
                file_path,
                capacity_bytes,
            },
            media,
        }
    }

    pub fn new_network(
        local_disk_id: String,
        disk_name: String,
        auto_mount: bool,
        server_addr: String,
        remote_disk_id: String,
        auth_material: String,
        capacity_bytes: u64,
        configured_read_only: bool,
        source_read_only: bool,
    ) -> Self {
        Self {
            local_disk_id,
            disk_name,
            auto_mount,
            configured_read_only,
            source_read_only,
            state: DiskRuntimeStatus::Invalid {
                reason: "网络盘会话未打开".to_string(),
            },
            media_config: DiskMediaConfig::Network {
                server_addr,
                remote_disk_id,
                auth_material,
                capacity_bytes,
            },
            media: None,
        }
    }

    pub fn local_disk_id(&self) -> &str {
        &self.local_disk_id
    }

    pub fn disk_name(&self) -> &str {
        &self.disk_name
    }

    pub fn auto_mount(&self) -> bool {
        self.auto_mount
    }

    pub fn set_user_config(
        &mut self,
        disk_name: String,
        auto_mount: bool,
        configured_read_only: bool,
    ) {
        self.disk_name = disk_name;
        self.auto_mount = auto_mount;
        self.configured_read_only = configured_read_only;
    }

    pub fn configured_read_only(&self) -> bool {
        self.configured_read_only
    }

    pub fn source_read_only(&self) -> bool {
        self.source_read_only
    }

    pub fn capacity_bytes(&self) -> u64 {
        match &self.media_config {
            DiskMediaConfig::Memory { capacity_bytes, .. } => *capacity_bytes,
            DiskMediaConfig::File { capacity_bytes, .. } => *capacity_bytes,
            DiskMediaConfig::Network { capacity_bytes, .. } => *capacity_bytes,
        }
    }

    pub fn status(&self) -> &DiskRuntimeStatus {
        &self.state
    }

    pub fn mounted_target_id(&self) -> Option<u32> {
        match self.status() {
            DiskRuntimeStatus::Mounted { target_id } => Some(*target_id),
            DiskRuntimeStatus::Unmounted | DiskRuntimeStatus::Invalid { .. } => None,
        }
    }

    pub fn invalid_reason(&self) -> Option<&str> {
        match self.status() {
            DiskRuntimeStatus::Invalid { reason } => Some(reason.as_str()),
            DiskRuntimeStatus::Unmounted | DiskRuntimeStatus::Mounted { .. } => None,
        }
    }

    pub fn is_memory(&self) -> bool {
        matches!(self.media_config, DiskMediaConfig::Memory { .. })
    }

    pub fn is_network(&self) -> bool {
        matches!(self.media_config, DiskMediaConfig::Network { .. })
    }

    pub fn file_path(&self) -> Option<&str> {
        match &self.media_config {
            DiskMediaConfig::Memory { .. } => None,
            DiskMediaConfig::File { file_path, .. } => Some(file_path.as_str()),
            DiskMediaConfig::Network { .. } => None,
        }
    }

    pub fn server_addr(&self) -> Option<&str> {
        match &self.media_config {
            DiskMediaConfig::Network { server_addr, .. } => Some(server_addr.as_str()),
            DiskMediaConfig::Memory { .. } | DiskMediaConfig::File { .. } => None,
        }
    }

    pub fn remote_disk_id(&self) -> Option<&str> {
        match &self.media_config {
            DiskMediaConfig::Network { remote_disk_id, .. } => Some(remote_disk_id.as_str()),
            DiskMediaConfig::Memory { .. } | DiskMediaConfig::File { .. } => None,
        }
    }

    pub fn auth_material(&self) -> Option<&str> {
        match &self.media_config {
            DiskMediaConfig::Network { auth_material, .. } => Some(auth_material.as_str()),
            DiskMediaConfig::Memory { .. } | DiskMediaConfig::File { .. } => None,
        }
    }

    pub fn media_snapshot(&self) -> DiskMediaConfig {
        self.media_config.clone()
    }

    pub fn take_media(&mut self) -> Option<Box<dyn Media>> {
        self.media.take()
    }

    pub fn restore_media(&mut self, media: Box<dyn Media>) {
        self.media = Some(media);
    }

    pub fn set_mounted(&mut self, target_id: u32) {
        self.state = DiskRuntimeStatus::Mounted { target_id };
    }

    pub fn set_unmounted(&mut self) {
        self.state = DiskRuntimeStatus::Unmounted;
    }

    pub fn set_memory_invalid(&mut self, reason: String) {
        self.state = DiskRuntimeStatus::Invalid { reason };
        self.media = None;
    }

    pub fn set_file_unmounted(&mut self, capacity_bytes: u64, source_read_only: bool) {
        if let DiskMediaConfig::File {
            capacity_bytes: current_capacity_bytes,
            ..
        } = &mut self.media_config
        {
            *current_capacity_bytes = capacity_bytes;
        }
        self.source_read_only = source_read_only;
        self.state = DiskRuntimeStatus::Unmounted;
    }

    pub fn set_file_invalid(&mut self, reason: String) {
        if let DiskMediaConfig::File {
            capacity_bytes: current_capacity_bytes,
            ..
        } = &mut self.media_config
        {
            *current_capacity_bytes = 0;
        }
        self.source_read_only = false;
        self.state = DiskRuntimeStatus::Invalid { reason };
        self.media = None;
    }

    pub fn set_network_unmounted(&mut self, capacity_bytes: u64, source_read_only: bool) {
        self.refresh_network_metadata(capacity_bytes, source_read_only);
        self.state = DiskRuntimeStatus::Unmounted;
    }

    pub fn refresh_network_metadata(&mut self, capacity_bytes: u64, source_read_only: bool) {
        if let DiskMediaConfig::Network {
            capacity_bytes: current_capacity_bytes,
            ..
        } = &mut self.media_config
        {
            *current_capacity_bytes = capacity_bytes;
        }
        self.source_read_only = source_read_only;
    }

    pub fn set_network_invalid(&mut self, reason: String) {
        self.state = DiskRuntimeStatus::Invalid { reason };
        self.media = None;
    }

    pub fn network_key(&self) -> Option<(String, String)> {
        match &self.media_config {
            DiskMediaConfig::Network {
                server_addr,
                remote_disk_id,
                ..
            } => Some((server_addr.clone(), remote_disk_id.clone())),
            DiskMediaConfig::Memory { .. } | DiskMediaConfig::File { .. } => None,
        }
    }

    pub fn snapshot(&self) -> DiskRuntimeSnapshot {
        DiskRuntimeSnapshot {
            local_disk_id: self.local_disk_id().to_string(),
            disk_name: self.disk_name().to_string(),
            auto_mount: self.auto_mount(),
            configured_read_only: self.configured_read_only(),
            source_read_only: self.source_read_only(),
            status: self.status().clone(),
            media: self.media_snapshot(),
        }
    }
}
