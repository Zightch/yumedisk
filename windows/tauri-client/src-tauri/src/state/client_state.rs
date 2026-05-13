use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::Mutex;

use backend_rust::BackendContext;

use crate::state::disk_runtime::DiskRuntimeStore;

pub struct ClientState {
    pub backend: BackendContext,
    pub disk_runtime_store: Mutex<DiskRuntimeStore>,
    is_exiting: AtomicBool,
}

impl Default for ClientState {
    fn default() -> Self {
        Self {
            backend: BackendContext::new(),
            disk_runtime_store: Mutex::new(DiskRuntimeStore::default()),
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
