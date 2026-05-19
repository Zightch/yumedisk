use std::sync::Arc;
use std::sync::Mutex;

use network_core::client::SessionCloseNotice;

use super::NetworkClientEvent;

#[derive(Debug, Default)]
pub struct PendingNetworkSignals {
    pending_events: Arc<Mutex<Vec<NetworkClientEvent>>>,
    pending_media_invalidations: Arc<Mutex<Vec<String>>>,
}

impl PendingNetworkSignals {
    pub fn session_notice_handler(
        &self,
        server_addr: &str,
    ) -> Arc<dyn Fn(SessionCloseNotice) + Send + Sync> {
        let pending_events = Arc::clone(&self.pending_events);
        let notice_server_addr = server_addr.to_string();
        Arc::new(move |notice| {
            if let Ok(mut events) = pending_events.lock() {
                events.push(NetworkClientEvent::SessionClosed {
                    server_addr: notice_server_addr.clone(),
                    session_id: notice.session_id,
                });
            }
        })
    }

    pub fn disconnect_handler(&self, server_addr: &str) -> Arc<dyn Fn() + Send + Sync> {
        let pending_events = Arc::clone(&self.pending_events);
        let disconnect_server_addr = server_addr.to_string();
        Arc::new(move || {
            if let Ok(mut events) = pending_events.lock() {
                events.push(NetworkClientEvent::ConnectionLost {
                    server_addr: disconnect_server_addr.clone(),
                });
            }
        })
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
}
