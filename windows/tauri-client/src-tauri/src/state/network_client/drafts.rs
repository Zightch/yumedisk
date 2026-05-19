use std::collections::HashMap;

use super::NetworkCreateDraft;

#[derive(Debug)]
pub struct DraftStore {
    entries: HashMap<String, NetworkCreateDraft>,
    next_draft_number: u64,
}

impl Default for DraftStore {
    fn default() -> Self {
        Self {
            entries: HashMap::new(),
            next_draft_number: 1,
        }
    }
}

impl DraftStore {
    pub fn insert(&mut self, draft: NetworkCreateDraft) {
        self.entries.insert(draft.draft_id.clone(), draft);
    }

    pub fn get(&self, draft_id: &str) -> Option<&NetworkCreateDraft> {
        self.entries.get(draft_id)
    }

    pub fn get_mut(&mut self, draft_id: &str) -> Option<&mut NetworkCreateDraft> {
        self.entries.get_mut(draft_id)
    }

    pub fn ids_for_server(&self, server_addr: &str) -> Vec<String> {
        self.entries
            .iter()
            .filter_map(|(draft_id, draft)| {
                if draft.server_addr == server_addr {
                    Some(draft_id.clone())
                } else {
                    None
                }
            })
            .collect()
    }

    pub fn remove(&mut self, draft_id: &str) -> Option<NetworkCreateDraft> {
        self.entries.remove(draft_id)
    }

    pub fn allocate_id(&mut self) -> String {
        let draft_id = format!("draft-{}", self.next_draft_number);
        self.next_draft_number += 1;
        draft_id
    }
}
