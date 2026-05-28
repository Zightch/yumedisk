use std::collections::BTreeMap;
use std::collections::BTreeSet;

use backend_rust::Media;

use super::DenseMem;
use super::LocalBindingKind;

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct SharedMemorySnapshot {
    pub smid: u64,
    pub size_bytes: u64,
    pub bound_targets: Vec<u32>,
}

#[derive(Debug, Clone)]
struct SharedMemoryEntry {
    media: DenseMem,
    size_bytes: u64,
    bound_targets: BTreeSet<u32>,
}

#[derive(Debug, Clone)]
pub struct SharedMemoryRegistry {
    next_smid: u64,
    shared: BTreeMap<u64, SharedMemoryEntry>,
    target_bindings: BTreeMap<u32, u64>,
}

impl Default for SharedMemoryRegistry {
    fn default() -> Self {
        Self {
            next_smid: 1,
            shared: BTreeMap::new(),
            target_bindings: BTreeMap::new(),
        }
    }
}

impl SharedMemoryRegistry {
    pub fn create_shared_memory(&mut self, size_bytes: u64) -> Result<u64, String> {
        let media = DenseMem::new(size_bytes).map_err(|error| error.to_string())?;
        let smid = self.next_smid;
        self.next_smid = self
            .next_smid
            .checked_add(1)
            .ok_or_else(|| "smid-overflow".to_string())?;
        self.shared.insert(
            smid,
            SharedMemoryEntry {
                media,
                size_bytes,
                bound_targets: BTreeSet::new(),
            },
        );
        Ok(smid)
    }

    pub fn prepare_dedicated_media(&self, size_bytes: u64) -> Result<(DenseMem, u64), String> {
        let media = DenseMem::new(size_bytes).map_err(|error| error.to_string())?;
        Ok((media, size_bytes))
    }

    pub fn prepare_shared_media(&self, smid: u64) -> Result<(DenseMem, u64), String> {
        let entry = self
            .shared
            .get(&smid)
            .ok_or_else(|| "smid-not-found".to_string())?;
        Ok((entry.media.clone(), entry.size_bytes))
    }

    pub fn register_target_binding(
        &mut self,
        target_id: u32,
        binding: LocalBindingKind,
    ) -> Result<(), String> {
        match binding {
            LocalBindingKind::Dedicated => Ok(()),
            LocalBindingKind::Shared { smid } => {
                let entry = self
                    .shared
                    .get_mut(&smid)
                    .ok_or_else(|| "smid-not-found".to_string())?;
                entry.bound_targets.insert(target_id);
                self.target_bindings.insert(target_id, smid);
                Ok(())
            }
        }
    }

    pub fn unbind_target(&mut self, target_id: u32) -> Option<u64> {
        let smid = self.target_bindings.remove(&target_id)?;
        if let Some(entry) = self.shared.get_mut(&smid) {
            entry.bound_targets.remove(&target_id);
        }
        Some(smid)
    }

    pub fn sibling_targets(&self, target_id: u32) -> Option<(u64, Vec<u32>)> {
        let smid = self.target_bindings.get(&target_id).copied()?;
        let entry = self.shared.get(&smid)?;
        let siblings = entry
            .bound_targets
            .iter()
            .copied()
            .filter(|bound_target| *bound_target != target_id)
            .collect::<Vec<_>>();
        Some((smid, siblings))
    }

    pub fn read_bound_target_bytes(
        &self,
        target_id: u32,
        offset: u64,
        length: usize,
    ) -> Result<Vec<u8>, String> {
        let entry = self.shared_entry_for_target(target_id)?;
        let mut bytes = vec![0; length];
        entry
            .media
            .read_locked(offset, &mut bytes)
            .map_err(|error| error.to_string())?;
        Ok(bytes)
    }

    pub fn write_bound_target_bytes(
        &self,
        target_id: u32,
        offset: u64,
        data: &[u8],
    ) -> Result<(), String> {
        let entry = self.shared_entry_for_target(target_id)?;
        entry
            .media
            .write_locked(offset, data)
            .map_err(|error| error.to_string())
    }

    pub fn remove_shared_memory(&mut self, smid: u64) -> Result<(), String> {
        let Some(entry) = self.shared.get(&smid) else {
            return Err("smid-not-found".to_string());
        };
        if !entry.bound_targets.is_empty() {
            return Err("smid-in-use".to_string());
        }
        self.shared.remove(&smid);
        Ok(())
    }

    pub fn remove_all_shared_memory(&mut self) -> Result<Vec<u64>, String> {
        if self
            .shared
            .values()
            .any(|entry| !entry.bound_targets.is_empty())
        {
            return Err("smid-in-use".to_string());
        }
        let removed = self.shared.keys().copied().collect::<Vec<_>>();
        self.shared.clear();
        Ok(removed)
    }

    pub fn snapshots(&self) -> Vec<SharedMemorySnapshot> {
        self.shared
            .iter()
            .map(|(smid, entry)| SharedMemorySnapshot {
                smid: *smid,
                size_bytes: entry.size_bytes,
                bound_targets: entry.bound_targets.iter().copied().collect(),
            })
            .collect()
    }

    fn shared_entry_for_target(&self, target_id: u32) -> Result<&SharedMemoryEntry, String> {
        let smid = self
            .target_bindings
            .get(&target_id)
            .copied()
            .ok_or_else(|| "target-not-shared-memory".to_string())?;
        self.shared
            .get(&smid)
            .ok_or_else(|| "smid-not-found".to_string())
    }
}

#[cfg(test)]
mod tests {
    use super::LocalBindingKind;
    use super::SharedMemoryRegistry;

    #[test]
    fn remove_shared_memory_rejects_bound_targets() {
        let mut registry = SharedMemoryRegistry::default();
        let smid = registry
            .create_shared_memory(4096)
            .expect("shared memory should be created");
        registry
            .register_target_binding(3, LocalBindingKind::Shared { smid })
            .expect("binding should succeed");

        let error = registry
            .remove_shared_memory(smid)
            .expect_err("smid should stay in use");
        assert_eq!(error, "smid-in-use");
    }

    #[test]
    fn sibling_targets_ignore_source_target() {
        let mut registry = SharedMemoryRegistry::default();
        let smid = registry
            .create_shared_memory(4096)
            .expect("shared memory should be created");
        registry
            .register_target_binding(3, LocalBindingKind::Shared { smid })
            .expect("binding should succeed");
        registry
            .register_target_binding(4, LocalBindingKind::Shared { smid })
            .expect("binding should succeed");

        let (_, siblings) = registry
            .sibling_targets(3)
            .expect("target should be bound to shared memory");
        assert_eq!(siblings, vec![4]);
    }

    #[test]
    fn remove_all_shared_memory_requires_every_smid_to_be_unused() {
        let mut registry = SharedMemoryRegistry::default();
        let smid = registry
            .create_shared_memory(4096)
            .expect("shared memory should be created");
        registry
            .register_target_binding(5, LocalBindingKind::Shared { smid })
            .expect("binding should succeed");

        let error = registry
            .remove_all_shared_memory()
            .expect_err("shared memory should stay in use");
        assert_eq!(error, "smid-in-use");
    }

    #[test]
    fn read_and_write_bound_target_bytes_share_same_backing_media() {
        let mut registry = SharedMemoryRegistry::default();
        let smid = registry
            .create_shared_memory(4096)
            .expect("shared memory should be created");
        registry
            .register_target_binding(3, LocalBindingKind::Shared { smid })
            .expect("binding should succeed");
        registry
            .register_target_binding(4, LocalBindingKind::Shared { smid })
            .expect("binding should succeed");

        registry
            .write_bound_target_bytes(3, 8, &[0xAA, 0xBB, 0xCC, 0xDD])
            .expect("write should succeed");

        let bytes = registry
            .read_bound_target_bytes(4, 8, 4)
            .expect("read should succeed");
        assert_eq!(bytes, vec![0xAA, 0xBB, 0xCC, 0xDD]);
    }

    #[test]
    fn read_bound_target_bytes_rejects_unbound_targets() {
        let registry = SharedMemoryRegistry::default();
        let error = registry
            .read_bound_target_bytes(9, 0, 4)
            .expect_err("unbound target should fail");
        assert_eq!(error, "target-not-shared-memory");
    }
}
