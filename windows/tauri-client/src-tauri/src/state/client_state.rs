use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::Mutex;

use backend_rust::BackendContext;

use crate::state::disk_catalog::DiskCatalogState;
use crate::state::network_client::NetworkClientState;

pub struct ClientState {
    pub backend: BackendContext,
    pub disk_catalog: Mutex<DiskCatalogState>,
    pub network_client: Mutex<NetworkClientState>,
    runtime_rescan_running: Mutex<bool>,
    is_exiting: AtomicBool,
}

impl Default for ClientState {
    fn default() -> Self {
        Self {
            backend: BackendContext::new(),
            disk_catalog: Mutex::new(DiskCatalogState::default()),
            network_client: Mutex::new(NetworkClientState::default()),
            runtime_rescan_running: Mutex::new(false),
            is_exiting: AtomicBool::new(false),
        }
    }
}

impl ClientState {
    pub fn try_begin_runtime_rescan(&self) -> bool {
        let mut running = self
            .runtime_rescan_running
            .lock()
            .expect("runtime rescan mutex should not be poisoned");
        if *running {
            return false;
        }

        *running = true;
        true
    }

    pub fn finish_runtime_rescan(&self) {
        let mut running = self
            .runtime_rescan_running
            .lock()
            .expect("runtime rescan mutex should not be poisoned");
        *running = false;
    }

    pub fn is_runtime_rescan_running(&self) -> bool {
        *self
            .runtime_rescan_running
            .lock()
            .expect("runtime rescan mutex should not be poisoned")
    }

    pub fn mark_exiting(&self) {
        self.is_exiting.store(true, Ordering::SeqCst);
    }

    pub fn is_exiting(&self) -> bool {
        self.is_exiting.load(Ordering::SeqCst)
    }
}
