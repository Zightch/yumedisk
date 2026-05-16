use std::sync::Arc;

use super::disk_session::DiskSession;
use super::error::NetworkClientError;
use super::gateway_connection::GatewayConnection;

#[derive(Debug, Clone)]
pub struct SessionOpener {
    connection: Arc<GatewayConnection>,
}

impl SessionOpener {
    pub fn new(connection: Arc<GatewayConnection>) -> Self {
        Self { connection }
    }

    pub fn open(&self, disk_id: impl Into<String>) -> Result<DiskSession, NetworkClientError> {
        let disk_id = disk_id.into();
        if disk_id.is_empty() {
            return Err(NetworkClientError::InvalidArgument("disk_id"));
        }
        if !self.connection.is_authorized(&disk_id) {
            return Err(NetworkClientError::UnauthorizedDisk { disk_id });
        }

        Err(NetworkClientError::Unimplemented(
            "B4.2 session opening will be implemented in B3/B4.2",
        ))
    }
}
