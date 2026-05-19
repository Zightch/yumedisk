use std::collections::HashMap;
use std::sync::Arc;

use network_core::client::GatewayConnection;
use network_core::client::NetworkClientError;
use network_core::transport::TransportEndpoint;

use super::pending_events::PendingNetworkSignals;

#[derive(Debug, Default)]
pub struct ConnectionPool {
    entries: HashMap<String, Arc<GatewayConnection>>,
}

impl ConnectionPool {
    pub fn connection(&self, server_addr: &str) -> Option<Arc<GatewayConnection>> {
        self.entries.get(server_addr).and_then(|connection| {
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
        signals: &PendingNetworkSignals,
    ) -> Result<Arc<GatewayConnection>, NetworkClientError> {
        if let Some(connection) = self.connection(server_addr) {
            return Ok(connection);
        }

        let connection = GatewayConnection::new(TransportEndpoint::new(server_addr.to_string()));
        connection.set_session_notice_handler(Some(signals.session_notice_handler(server_addr)));
        connection.set_disconnect_handler(Some(signals.disconnect_handler(server_addr)));
        connection.connect()?;
        self.entries
            .insert(server_addr.to_string(), Arc::clone(&connection));
        Ok(connection)
    }

    pub fn release_connection_after_session_close(&mut self, server_addr: &str) {
        let should_close = self
            .entries
            .get(server_addr)
            .map(|connection| connection.should_close_after_session_close())
            .unwrap_or(false);

        if let Some(connection) = self.entries.get(server_addr).cloned() {
            if should_close {
                let _ = connection.close();
            }
            if !connection.is_connected() {
                self.entries.remove(server_addr);
            }
        }
    }

    pub fn cleanup_connection_if_idle(&mut self, server_addr: &str) {
        if let Some(connection) = self.entries.get(server_addr).cloned() {
            if connection.should_close_after_session_close() {
                let _ = connection.close();
            }
            if !connection.is_connected() {
                self.entries.remove(server_addr);
            }
        }
    }
}
