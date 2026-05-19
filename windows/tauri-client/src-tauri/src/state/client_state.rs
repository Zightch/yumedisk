use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::Mutex;

use backend_rust::BackendContext;

use crate::state::disk_catalog::DiskCatalogState;
use crate::state::network_client::NetworkClientState;

pub struct ClientState {
    pub backend: BackendContext,
    pub disk_catalog: Mutex<DiskCatalogState>,
    pub network_client: Mutex<NetworkClientState>,
    is_exiting: AtomicBool,
}

impl Default for ClientState {
    fn default() -> Self {
        Self {
            backend: BackendContext::new(),
            disk_catalog: Mutex::new(DiskCatalogState::default()),
            network_client: Mutex::new(NetworkClientState::default()),
            is_exiting: AtomicBool::new(false),
        }
    }
}

impl ClientState {
    pub fn mark_exiting(&self) {
        self.is_exiting.store(true, Ordering::SeqCst);
    }

    pub fn is_exiting(&self) -> bool {
        self.is_exiting.load(Ordering::SeqCst)
    }
}
