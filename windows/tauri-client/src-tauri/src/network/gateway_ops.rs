use std::sync::Arc;
use std::sync::Mutex;

use network_core::client::AuthGrant;
use network_core::client::ConnectionAuthenticator;
use network_core::client::DiskSession;
use network_core::client::GatewayConnection;
use network_core::client::NetworkClientError;
use network_core::client::SessionDescriber;
use network_core::client::SessionMetadata;
use network_core::client::SessionOpener;

use crate::state::network_client::NetworkClientState;

use super::lock_network_client;

pub fn acquire_connection(
    network_client_mutex: &Mutex<NetworkClientState>,
    server_addr: &str,
) -> Result<Arc<GatewayConnection>, NetworkClientError> {
    let mut network_client = lock_network_client(network_client_mutex);
    network_client.acquire_connection(server_addr)
}

pub fn authenticate(
    connection: Arc<GatewayConnection>,
    claim_code: &str,
) -> Result<AuthGrant, NetworkClientError> {
    ConnectionAuthenticator::new(connection).authenticate(claim_code)
}

pub fn open_session(
    connection: Arc<GatewayConnection>,
    grant: &AuthGrant,
) -> Result<DiskSession, NetworkClientError> {
    let session_id = SessionOpener::new(Arc::clone(&connection)).open(grant)?;
    DiskSession::new(connection, session_id)
}

pub fn describe_session(
    connection: Arc<GatewayConnection>,
    session_id: u64,
) -> Result<SessionMetadata, NetworkClientError> {
    SessionDescriber::new(connection).describe(session_id)
}
