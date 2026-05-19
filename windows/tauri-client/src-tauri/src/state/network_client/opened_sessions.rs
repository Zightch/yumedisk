use std::collections::HashMap;

use super::NetworkDiskKey;
use super::OpenedNetworkDiskSession;

#[derive(Debug, Default)]
pub struct OpenedSessionTable {
    entries: HashMap<NetworkDiskKey, OpenedNetworkDiskSession>,
}

impl OpenedSessionTable {
    pub fn insert(&mut self, session: OpenedNetworkDiskSession) {
        self.entries.insert(session.key.clone(), session);
    }

    pub fn get(&self, key: &NetworkDiskKey) -> Option<OpenedNetworkDiskSession> {
        self.entries.get(key).cloned()
    }

    pub fn remove(&mut self, key: &NetworkDiskKey) -> Option<OpenedNetworkDiskSession> {
        self.entries.remove(key)
    }

    pub fn find_key_by_session_id(
        &self,
        server_addr: &str,
        session_id: u64,
    ) -> Option<NetworkDiskKey> {
        self.entries.iter().find_map(|(key, session)| {
            if key.server_addr == server_addr && session.session.session_id() == session_id {
                Some(key.clone())
            } else {
                None
            }
        })
    }

    pub fn keys_for_server(&self, server_addr: &str) -> Vec<NetworkDiskKey> {
        self.entries
            .keys()
            .filter(|key| key.server_addr == server_addr)
            .cloned()
            .collect()
    }
}
