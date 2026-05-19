use std::collections::BTreeMap;
use std::sync::Mutex;

use backend_rust::BackendContext;
use network_core::client::DiskSession;
use network_core::client::NetworkClientError;
use network_core::client::SessionMetadata;

use crate::state::disk_runtime::DiskRuntimeStore;
use crate::state::network_client::NetworkClientState;
use crate::state::network_client::NetworkDiskKey;

use super::lock_network_client;

pub fn remove_draft_sessions_by_session_id(
    network_client: &mut NetworkClientState,
    server_addr: &str,
    session_id: u64,
) -> Vec<DiskSession> {
    let draft_ids = network_client.draft_ids_for_server(server_addr);
    let mut removed = Vec::new();

    for draft_id in draft_ids {
        let remote_disk_id = network_client.draft(&draft_id).and_then(|draft| {
            draft.items.iter().find_map(|(remote_disk_id, item)| {
                if item.session.session_id() == session_id {
                    Some(remote_disk_id.clone())
                } else {
                    None
                }
            })
        });
        let Some(remote_disk_id) = remote_disk_id else {
            continue;
        };

        if let Some(draft) = network_client.draft_mut(&draft_id) {
            if let Some(item) = draft.remove_item(&remote_disk_id) {
                removed.push(item.session);
            }
        }
    }

    removed
}

pub fn remove_all_draft_sessions_for_server(
    network_client: &mut NetworkClientState,
    server_addr: &str,
) -> Vec<DiskSession> {
    let draft_ids = network_client.draft_ids_for_server(server_addr);
    let mut removed = Vec::new();

    for draft_id in draft_ids {
        let items = if let Some(draft) = network_client.draft_mut(&draft_id) {
            std::mem::take(&mut draft.items)
        } else {
            BTreeMap::new()
        };
        for item in items.into_values() {
            removed.push(item.session);
        }
    }

    removed
}

pub fn invalidate_runtime_by_key(
    backend: &BackendContext,
    runtime_store: &mut DiskRuntimeStore,
    network_client_mutex: &Mutex<NetworkClientState>,
    key: &NetworkDiskKey,
    reason: &str,
) {
    let opened_session = {
        let mut network_client = lock_network_client(network_client_mutex);
        network_client.remove_opened_session(key)
    };
    if let Some(opened_session) = opened_session {
        let _ = close_session_for_cleanup(&opened_session.session);
    }

    for runtime in runtime_store.runtimes_mut() {
        let matches_key = runtime.server_addr() == Some(key.server_addr.as_str())
            && runtime.remote_disk_id() == Some(key.remote_disk_id.as_str());
        if !matches_key {
            continue;
        }

        if let Some(target_id) = runtime.mounted_target_id() {
            let mut error_text = String::new();
            if let Some(media) =
                backend.remove_managed_disk_with_media(target_id, Some(&mut error_text))
            {
                drop(media);
            }
        }
        runtime.set_network_invalid(reason.to_string());
    }
}

pub fn invalidate_runtime_by_local_disk_id(
    backend: &BackendContext,
    runtime_store: &mut DiskRuntimeStore,
    network_client_mutex: &Mutex<NetworkClientState>,
    local_disk_id: &str,
    reason: &str,
) -> bool {
    let Some((server_addr, remote_disk_id)) = runtime_store
        .find_runtime(local_disk_id)
        .and_then(|runtime| runtime.network_key())
    else {
        return false;
    };

    let key = NetworkDiskKey::new(server_addr, remote_disk_id);
    invalidate_runtime_by_key(backend, runtime_store, network_client_mutex, &key, reason);

    {
        let mut network_client = lock_network_client(network_client_mutex);
        network_client.release_connection_after_session_close(&key.server_addr);
    }

    true
}

pub fn set_network_runtime_unmounted(
    backend: &BackendContext,
    runtime_store: &mut DiskRuntimeStore,
    local_disk_id: &str,
    metadata: SessionMetadata,
) {
    if let Some(runtime) = runtime_store.find_runtime_mut(local_disk_id) {
        detach_managed_disk_if_mounted(backend, runtime);
        runtime.set_network_unmounted(metadata.disk_size_bytes, metadata.read_only);
    }
}

pub fn refresh_network_runtime(
    runtime_store: &mut DiskRuntimeStore,
    local_disk_id: &str,
    metadata: SessionMetadata,
) {
    if let Some(runtime) = runtime_store.find_runtime_mut(local_disk_id) {
        runtime.refresh_network_metadata(metadata.disk_size_bytes, metadata.read_only);
    }
}

pub fn set_network_runtime_invalid(
    backend: &BackendContext,
    runtime_store: &mut DiskRuntimeStore,
    local_disk_id: &str,
    reason: &str,
) {
    if let Some(runtime) = runtime_store.find_runtime_mut(local_disk_id) {
        detach_managed_disk_if_mounted(backend, runtime);
        runtime.set_network_invalid(reason.to_string());
    }
}

pub fn close_session_for_cleanup(session: &DiskSession) -> bool {
    match session.close() {
        Ok(()) | Err(NetworkClientError::SessionUnavailable) => true,
        Err(_) => false,
    }
}

fn detach_managed_disk_if_mounted(
    backend: &BackendContext,
    runtime: &mut crate::state::disk_runtime::DiskRuntime,
) {
    let Some(target_id) = runtime.mounted_target_id() else {
        return;
    };

    let mut error_text = String::new();
    if let Some(media) = backend.remove_managed_disk_with_media(target_id, Some(&mut error_text)) {
        drop(media);
    }
}
