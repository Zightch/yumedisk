use std::sync::Mutex;

use backend_rust::BackendContext;

use crate::state::disk_runtime::DiskRuntimeStore;
use crate::state::network_client::NetworkClientEvent;
use crate::state::network_client::NetworkClientState;

use super::NETWORK_CONNECTION_UNAVAILABLE_REASON;
use super::NETWORK_SESSION_MISSING_REASON;
use super::cleanup;
use super::lock_network_client;

pub fn sync_pending_events(
    backend: &BackendContext,
    runtime_store: &mut DiskRuntimeStore,
    network_client_mutex: &Mutex<NetworkClientState>,
) -> bool {
    let events = {
        let network_client = lock_network_client(network_client_mutex);
        network_client.drain_events()
    };
    let invalidations = {
        let network_client = lock_network_client(network_client_mutex);
        network_client.drain_media_invalidations()
    };

    let mut changed = false;
    for event in events {
        changed |= match event {
            NetworkClientEvent::SessionClosed {
                server_addr,
                session_id,
            } => {
                let opened_session_key = {
                    let network_client = lock_network_client(network_client_mutex);
                    network_client.find_opened_session_by_session_id(&server_addr, session_id)
                };
                if let Some(key) = opened_session_key.as_ref() {
                    cleanup::invalidate_runtime_by_key(
                        backend,
                        runtime_store,
                        network_client_mutex,
                        key,
                        NETWORK_SESSION_MISSING_REASON,
                    );
                }

                let draft_sessions = {
                    let mut network_client = lock_network_client(network_client_mutex);
                    cleanup::remove_draft_sessions_by_session_id(
                        &mut network_client,
                        &server_addr,
                        session_id,
                    )
                };
                for session in draft_sessions {
                    let _ = cleanup::close_session_for_cleanup(&session);
                }
                {
                    let mut network_client = lock_network_client(network_client_mutex);
                    network_client.release_connection_after_session_close(&server_addr);
                }
                true
            }
            NetworkClientEvent::ConnectionLost { server_addr } => {
                let opened_session_keys = {
                    let network_client = lock_network_client(network_client_mutex);
                    network_client.opened_session_keys_for_server(&server_addr)
                };

                for key in opened_session_keys {
                    cleanup::invalidate_runtime_by_key(
                        backend,
                        runtime_store,
                        network_client_mutex,
                        &key,
                        NETWORK_CONNECTION_UNAVAILABLE_REASON,
                    );
                }

                let draft_sessions = {
                    let mut network_client = lock_network_client(network_client_mutex);
                    cleanup::remove_all_draft_sessions_for_server(
                        &mut network_client,
                        &server_addr,
                    )
                };
                for session in draft_sessions {
                    let _ = cleanup::close_session_for_cleanup(&session);
                }
                {
                    let mut network_client = lock_network_client(network_client_mutex);
                    network_client.cleanup_connection_if_idle(&server_addr);
                }
                true
            }
        };
    }

    for local_disk_id in invalidations {
        changed |= cleanup::invalidate_runtime_by_local_disk_id(
            backend,
            runtime_store,
            network_client_mutex,
            &local_disk_id,
            NETWORK_SESSION_MISSING_REASON,
        );
    }

    changed
}

#[cfg(test)]
mod tests {
    use std::sync::Mutex;

    use backend_rust::BackendContext;

    use super::sync_pending_events;
    use crate::network::NETWORK_SESSION_MISSING_REASON;
    use crate::state::disk_runtime::DiskRuntime;
    use crate::state::disk_runtime::DiskRuntimeStatus;
    use crate::state::disk_runtime::DiskRuntimeStore;
    use crate::state::network_client::NetworkClientState;

    #[test]
    fn media_invalidation_marks_unmounted_runtime_invalid() {
        let backend = BackendContext::default();
        let network_client = NetworkClientState::default();
        let invalidate = network_client.media_invalidation_handler("disk-1");
        let network_client_mutex = Mutex::new(network_client);

        let mut runtime_store = DiskRuntimeStore::default();
        let mut runtime = DiskRuntime::new_network(
            "disk-1".to_string(),
            "network-disk".to_string(),
            false,
            "127.0.0.1:9002".to_string(),
            "A1b2C3d4E5f6G7h8".to_string(),
            "claim-1".to_string(),
            4096,
            false,
        );
        runtime.set_network_unmounted(4096, false);
        runtime_store.insert_runtime(runtime);

        invalidate();

        let changed = sync_pending_events(&backend, &mut runtime_store, &network_client_mutex);
        assert!(changed);

        let runtime = runtime_store
            .find_runtime("disk-1")
            .expect("runtime should exist");
        assert_eq!(
            runtime.status(),
            &DiskRuntimeStatus::Invalid {
                reason: NETWORK_SESSION_MISSING_REASON.to_string(),
            }
        );
    }
}
