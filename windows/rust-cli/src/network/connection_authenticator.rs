use std::sync::Arc;

use super::error::NetworkClientError;
use super::gateway_connection::GatewayConnection;

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct AuthGrant {
    pub disk_id: String,
}

#[derive(Debug, Clone)]
pub struct ConnectionAuthenticator {
    connection: Arc<GatewayConnection>,
}

impl ConnectionAuthenticator {
    pub fn new(connection: Arc<GatewayConnection>) -> Self {
        Self { connection }
    }

    pub fn connection(&self) -> &Arc<GatewayConnection> {
        &self.connection
    }

    pub fn authenticate(
        &self,
        disk_id: impl Into<String>,
        _claim_code: &str,
    ) -> Result<AuthGrant, NetworkClientError> {
        let disk_id = disk_id.into();
        if disk_id.is_empty() {
            return Err(NetworkClientError::InvalidArgument("disk_id"));
        }

        Err(NetworkClientError::Unimplemented(
            "B4.1 authenticate will be implemented in B3/B4.1",
        ))
    }
}
