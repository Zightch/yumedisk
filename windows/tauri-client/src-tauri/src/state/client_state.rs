use std::sync::Mutex;

use backend_rust::BackendContext;

use crate::state::disk_store::DiskStore;

pub struct ClientState {
    pub backend: BackendContext,
    pub disk_store: Mutex<DiskStore>,
}

impl Default for ClientState {
    fn default() -> Self {
        Self {
            backend: BackendContext::new(),
            disk_store: Mutex::new(DiskStore::default()),
        }
    }
}
