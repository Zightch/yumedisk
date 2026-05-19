mod connection_pool;
mod drafts;
mod opened_sessions;
mod pending_events;

use std::collections::BTreeMap;
use std::hash::Hash;
use std::sync::Arc;

use network_core::client::DiskSession;
use network_core::client::GatewayConnection;
use network_core::client::NetworkClientError;
use network_core::client::SessionMetadata;

use self::connection_pool::ConnectionPool;
use self::drafts::DraftStore;
use self::opened_sessions::OpenedSessionTable;
use self::pending_events::PendingNetworkSignals;

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

#[derive(Debug, Default)]
pub struct NetworkClientState {
    connection_pool: ConnectionPool,
    opened_sessions: OpenedSessionTable,
    drafts: DraftStore,
    pending_signals: PendingNetworkSignals,
}

impl NetworkClientState {
    pub fn acquire_connection(
        &mut self,
        server_addr: &str,
    ) -> Result<Arc<GatewayConnection>, NetworkClientError> {
        self.connection_pool
            .acquire_connection(server_addr, &self.pending_signals)
    }

    pub fn insert_opened_session(&mut self, session: OpenedNetworkDiskSession) {
        self.opened_sessions.insert(session);
    }

    pub fn opened_session(&self, key: &NetworkDiskKey) -> Option<OpenedNetworkDiskSession> {
        self.opened_sessions.get(key)
    }

    pub fn remove_opened_session(
        &mut self,
        key: &NetworkDiskKey,
    ) -> Option<OpenedNetworkDiskSession> {
        self.opened_sessions.remove(key)
    }

    pub fn find_opened_session_by_session_id(
        &self,
        server_addr: &str,
        session_id: u64,
    ) -> Option<NetworkDiskKey> {
        self.opened_sessions
            .find_key_by_session_id(server_addr, session_id)
    }

    pub fn opened_session_keys_for_server(&self, server_addr: &str) -> Vec<NetworkDiskKey> {
        self.opened_sessions.keys_for_server(server_addr)
    }

    pub fn insert_draft(&mut self, draft: NetworkCreateDraft) {
        self.drafts.insert(draft);
    }

    pub fn draft(&self, draft_id: &str) -> Option<&NetworkCreateDraft> {
        self.drafts.get(draft_id)
    }

    pub fn draft_mut(&mut self, draft_id: &str) -> Option<&mut NetworkCreateDraft> {
        self.drafts.get_mut(draft_id)
    }

    pub fn draft_ids_for_server(&self, server_addr: &str) -> Vec<String> {
        self.drafts.ids_for_server(server_addr)
    }

    pub fn remove_draft(&mut self, draft_id: &str) -> Option<NetworkCreateDraft> {
        self.drafts.remove(draft_id)
    }

    pub fn allocate_draft_id(&mut self) -> String {
        self.drafts.allocate_id()
    }

    pub fn drain_events(&self) -> Vec<NetworkClientEvent> {
        self.pending_signals.drain_events()
    }

    pub fn media_invalidation_handler(
        &self,
        local_disk_id: impl Into<String>,
    ) -> Arc<dyn Fn() + Send + Sync> {
        self.pending_signals
            .media_invalidation_handler(local_disk_id)
    }

    pub fn drain_media_invalidations(&self) -> Vec<String> {
        self.pending_signals.drain_media_invalidations()
    }

    pub fn release_connection_after_session_close(&mut self, server_addr: &str) {
        self.connection_pool
            .release_connection_after_session_close(server_addr);
    }

    pub fn cleanup_connection_if_idle(&mut self, server_addr: &str) {
        self.connection_pool.cleanup_connection_if_idle(server_addr);
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
