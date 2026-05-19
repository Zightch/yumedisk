use std::collections::BTreeMap;
use std::collections::HashMap;
use std::hash::Hash;
use std::sync::Arc;
use std::sync::Mutex;

use network_core::client::DiskSession;
use network_core::client::GatewayConnection;
use network_core::client::NetworkClientError;
use network_core::client::SessionMetadata;
use network_core::transport::TransportEndpoint;

#[derive(Debug, Clone, PartialEq, Eq, Hash)]
pub struct NetworkDiskKey {
    pub server_addr: String,
    pub remote_disk_id: String,
}

impl NetworkDiskKey {
    pub fn new(server_addr: impl Into<String>, remote_disk_id: impl Into<String>) -> Self {
        Self {
            server_addr: server_addr.into(),
            remote_disk_id: remote_disk_id.into(),
        }
    }
}

#[derive(Debug, Clone)]
pub enum NetworkClientEvent {
    SessionClosed {
        server_addr: String,
        session_id: u64,
    },
    ConnectionLost {
        server_addr: String,
    },
}

#[derive(Debug, Clone)]
pub struct OpenedNetworkDiskSession {
    pub key: NetworkDiskKey,
    pub session: DiskSession,
    pub metadata: SessionMetadata,
}

#[derive(Debug, Clone)]
pub struct NetworkDraftItem {
    pub key: NetworkDiskKey,
    pub disk_name: String,
    pub auth_material: String,
    pub session: DiskSession,
    pub metadata: SessionMetadata,
}

#[derive(Debug, Clone)]
pub struct NetworkCreateDraft {
    pub draft_id: String,
    pub server_addr: String,
    pub connection: Arc<GatewayConnection>,
    pub items: BTreeMap<String, NetworkDraftItem>,
}

#[derive(Debug)]
pub struct NetworkClientState {
    connection_pool: HashMap<String, Arc<GatewayConnection>>,
    opened_disk_sessions: HashMap<NetworkDiskKey, OpenedNetworkDiskSession>,
    network_create_drafts: HashMap<String, NetworkCreateDraft>,
    pending_events: Arc<Mutex<Vec<NetworkClientEvent>>>,
    pending_media_invalidations: Arc<Mutex<Vec<String>>>,
    next_draft_number: u64,
}

impl Default for NetworkClientState {
    fn default() -> Self {
        Self {
            connection_pool: HashMap::new(),
            opened_disk_sessions: HashMap::new(),
            network_create_drafts: HashMap::new(),
            pending_events: Arc::new(Mutex::new(Vec::new())),
            pending_media_invalidations: Arc::new(Mutex::new(Vec::new())),
            next_draft_number: 1,
        }
    }
}

impl NetworkClientState {
    pub fn connection(&self, server_addr: &str) -> Option<Arc<GatewayConnection>> {
        self.connection_pool
            .get(server_addr)
            .and_then(|connection| {
                if connection.is_connected() {
                    Some(Arc::clone(connection))
                } else {
                    None
                }
            })
    }

    pub fn acquire_connection(
        &mut self,
        server_addr: &str,
    ) -> Result<Arc<GatewayConnection>, NetworkClientError> {
        if let Some(connection) = self.connection(server_addr) {
            return Ok(connection);
        }

        let connection = GatewayConnection::new(TransportEndpoint::new(server_addr.to_string()));
        let pending_events = Arc::clone(&self.pending_events);
        let notice_server_addr = server_addr.to_string();
        connection.set_session_notice_handler(Some(Arc::new(move |notice| {
            if let Ok(mut events) = pending_events.lock() {
                events.push(NetworkClientEvent::SessionClosed {
                    server_addr: notice_server_addr.clone(),
                    session_id: notice.session_id,
                });
            }
        })));

        let pending_events = Arc::clone(&self.pending_events);
        let disconnect_server_addr = server_addr.to_string();
        connection.set_disconnect_handler(Some(Arc::new(move || {
            if let Ok(mut events) = pending_events.lock() {
                events.push(NetworkClientEvent::ConnectionLost {
                    server_addr: disconnect_server_addr.clone(),
                });
            }
        })));

        connection.connect()?;
        self.connection_pool
            .insert(server_addr.to_string(), Arc::clone(&connection));
        Ok(connection)
    }

    pub fn insert_opened_session(&mut self, session: OpenedNetworkDiskSession) {
        self.opened_disk_sessions
            .insert(session.key.clone(), session);
    }

    pub fn opened_session(&self, key: &NetworkDiskKey) -> Option<OpenedNetworkDiskSession> {
        self.opened_disk_sessions.get(key).cloned()
    }

    pub fn remove_opened_session(
        &mut self,
        key: &NetworkDiskKey,
    ) -> Option<OpenedNetworkDiskSession> {
        self.opened_disk_sessions.remove(key)
    }

    pub fn find_opened_session_by_session_id(
        &self,
        server_addr: &str,
        session_id: u64,
    ) -> Option<NetworkDiskKey> {
        self.opened_disk_sessions.iter().find_map(|(key, session)| {
            if key.server_addr == server_addr && session.session.session_id() == session_id {
                Some(key.clone())
            } else {
                None
            }
        })
    }

    pub fn opened_session_keys_for_server(&self, server_addr: &str) -> Vec<NetworkDiskKey> {
        self.opened_disk_sessions
            .keys()
            .filter(|key| key.server_addr == server_addr)
            .cloned()
            .collect()
    }

    pub fn insert_draft(&mut self, draft: NetworkCreateDraft) {
        self.network_create_drafts
            .insert(draft.draft_id.clone(), draft);
    }

    pub fn draft(&self, draft_id: &str) -> Option<&NetworkCreateDraft> {
        self.network_create_drafts.get(draft_id)
    }

    pub fn draft_mut(&mut self, draft_id: &str) -> Option<&mut NetworkCreateDraft> {
        self.network_create_drafts.get_mut(draft_id)
    }

    pub fn draft_ids_for_server(&self, server_addr: &str) -> Vec<String> {
        self.network_create_drafts
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

    pub fn remove_draft(&mut self, draft_id: &str) -> Option<NetworkCreateDraft> {
        self.network_create_drafts.remove(draft_id)
    }

    pub fn allocate_draft_id(&mut self) -> String {
        let draft_id = format!("draft-{}", self.next_draft_number);
        self.next_draft_number += 1;
        draft_id
    }

    pub fn drain_events(&self) -> Vec<NetworkClientEvent> {
        let mut events = self
            .pending_events
            .lock()
            .expect("network pending events poisoned");
        events.drain(..).collect()
    }

    pub fn media_invalidation_handler(
        &self,
        local_disk_id: impl Into<String>,
    ) -> Arc<dyn Fn() + Send + Sync> {
        let pending_media_invalidations = Arc::clone(&self.pending_media_invalidations);
        let local_disk_id = local_disk_id.into();

        Arc::new(move || {
            if let Ok(mut invalidations) = pending_media_invalidations.lock() {
                invalidations.push(local_disk_id.clone());
            }
        })
    }

    pub fn drain_media_invalidations(&self) -> Vec<String> {
        let mut invalidations = self
            .pending_media_invalidations
            .lock()
            .expect("network media invalidations poisoned")
            .drain(..)
            .collect::<Vec<_>>();
        invalidations.sort();
        invalidations.dedup();
        invalidations
    }

    pub fn release_connection_after_session_close(&mut self, server_addr: &str) {
        let should_close = self
            .connection_pool
            .get(server_addr)
            .map(|connection| connection.should_close_after_session_close())
            .unwrap_or(false);

        if let Some(connection) = self.connection_pool.get(server_addr).cloned() {
            if should_close {
                let _ = connection.close();
            }
            if !connection.is_connected() {
                self.connection_pool.remove(server_addr);
            }
        }
    }

    pub fn cleanup_connection_if_idle(&mut self, server_addr: &str) {
        if let Some(connection) = self.connection_pool.get(server_addr).cloned() {
            if connection.should_close_after_session_close() {
                let _ = connection.close();
            }
            if !connection.is_connected() {
                self.connection_pool.remove(server_addr);
            }
        }
    }
}

impl NetworkCreateDraft {
    pub fn new(draft_id: String, server_addr: String, connection: Arc<GatewayConnection>) -> Self {
        Self {
            draft_id,
            server_addr,
            connection,
            items: BTreeMap::new(),
        }
    }

    pub fn insert_item(&mut self, item: NetworkDraftItem) {
        self.items.insert(item.key.remote_disk_id.clone(), item);
    }

    pub fn remove_item(&mut self, remote_disk_id: &str) -> Option<NetworkDraftItem> {
        self.items.remove(remote_disk_id)
    }
}

#[cfg(test)]
mod tests {
    use super::NetworkClientState;

    #[test]
    fn media_invalidations_drain_once_and_deduplicate() {
        let network_client = NetworkClientState::default();

        let invalidate_disk_two = network_client.media_invalidation_handler("disk-2");
        let invalidate_disk_one = network_client.media_invalidation_handler("disk-1");

        invalidate_disk_two();
        invalidate_disk_one();
        invalidate_disk_two();

        assert_eq!(
            network_client.drain_media_invalidations(),
            vec!["disk-1".to_string(), "disk-2".to_string()]
        );
        assert!(network_client.drain_media_invalidations().is_empty());
    }
}
