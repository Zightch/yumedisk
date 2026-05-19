use std::sync::Mutex;

use backend_rust::BackendContext;

use crate::network::event_reconciler;
use crate::state::disk_runtime::DiskRuntimeStore;
use crate::state::network_client::NetworkClientState;

pub fn sync_runtime_state(
    backend: &BackendContext,
    runtime_store: &mut DiskRuntimeStore,
    network_client_mutex: &Mutex<NetworkClientState>,
) -> bool {
    event_reconciler::sync_pending_events(backend, runtime_store, network_client_mutex)
}
