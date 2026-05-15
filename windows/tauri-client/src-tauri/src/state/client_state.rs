use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::Mutex;

use backend_rust::BackendContext;

use crate::state::disk_catalog::DiskCatalogState;

pub struct ClientState {
    pub backend: BackendContext,
    pub disk_catalog: Mutex<DiskCatalogState>,
    is_exiting: AtomicBool,
}

impl Default for ClientState {
    fn default() -> Self {
        Self {
            backend: BackendContext::new(),
            disk_catalog: Mutex::new(DiskCatalogState::default()),
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
