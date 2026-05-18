use crate::state::disk_runtime::DiskRuntimeStore;
use crate::state::disk_runtime::RemovedDiskRuntime;

pub struct PendingDiskDeletion {
    pub deletion_id: String,
    pub removed_runtime: RemovedDiskRuntime,
}

pub struct DiskCatalogState {
    runtime_store: DiskRuntimeStore,
    pending_deletions: Vec<PendingDiskDeletion>,
    next_deletion_number: u64,
}

impl Default for DiskCatalogState {
    fn default() -> Self {
        Self {
            runtime_store: DiskRuntimeStore::default(),
            pending_deletions: Vec::new(),
            next_deletion_number: 1,
        }
    }
}

impl DiskCatalogState {
    pub fn runtime_store(&self) -> &DiskRuntimeStore {
        &self.runtime_store
    }

    pub fn runtime_store_mut(&mut self) -> &mut DiskRuntimeStore {
        &mut self.runtime_store
    }

    pub fn replace_runtime_store(&mut self, runtime_store: DiskRuntimeStore) {
        self.runtime_store = runtime_store;
        self.pending_deletions.clear();
        self.next_deletion_number = 1;
    }

    pub fn remove_runtime(&mut self, local_disk_id: &str) -> Option<RemovedDiskRuntime> {
        self.runtime_store.remove_runtime(local_disk_id)
    }

    pub fn restore_removed_runtime(&mut self, removed_runtime: RemovedDiskRuntime) {
        self.runtime_store.restore_removed_runtime(removed_runtime);
    }

    pub fn stage_pending_deletion(&mut self, removed_runtime: RemovedDiskRuntime) -> String {
        let deletion_id = format!("deletion-{}", self.next_deletion_number);
        self.next_deletion_number += 1;
        self.pending_deletions.push(PendingDiskDeletion {
            deletion_id: deletion_id.clone(),
            removed_runtime,
        });
        deletion_id
    }

    pub fn take_pending_deletion(&mut self, deletion_id: &str) -> Option<PendingDiskDeletion> {
        let index = self
            .pending_deletions
            .iter()
            .position(|pending| pending.deletion_id == deletion_id)?;
        Some(self.pending_deletions.remove(index))
    }

    pub fn restore_pending_deletion(&mut self, pending_deletion: PendingDiskDeletion) {
        self.pending_deletions.push(pending_deletion);
    }

    pub fn commit_pending_deletion(&mut self, deletion_id: &str) -> Option<PendingDiskDeletion> {
        self.take_pending_deletion(deletion_id)
    }
}
