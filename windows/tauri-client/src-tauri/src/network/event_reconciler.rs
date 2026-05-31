use std::collections::BTreeSet;
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
    sync_pending_events_with_notify(backend, runtime_store, network_client_mutex, |target_id| {
        let _ = backend.notify_managed_disk_data_changed(target_id, None);
    })
}

fn sync_pending_events_with_notify<NotifyFn>(
    backend: &BackendContext,
    runtime_store: &mut DiskRuntimeStore,
    network_client_mutex: &Mutex<NetworkClientState>,
    mut notify_data_changed: NotifyFn,
) -> bool
where
    NotifyFn: FnMut(u32),
{
    let events = {
        let network_client = lock_network_client(network_client_mutex);
        network_client.drain_events()
    };
    let invalidations = {
        let network_client = lock_network_client(network_client_mutex);
        network_client.drain_media_invalidations()
    };

    let mut changed = false;
    let mut data_changed_keys = BTreeSet::<(String, String)>::new();
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
            NetworkClientEvent::SessionDataChanged {
                server_addr,
                session_id,
            } => {
                let opened_session_key = {
                    let network_client = lock_network_client(network_client_mutex);
                    network_client.find_opened_session_by_session_id(&server_addr, session_id)
                };
                if let Some(key) = opened_session_key {
                    data_changed_keys.insert((key.server_addr, key.remote_disk_id));
                }
                false
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
                    cleanup::remove_all_draft_sessions_for_server(&mut network_client, &server_addr)
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

    for (server_addr, remote_disk_id) in data_changed_keys {
        if let Some(target_id) =
            find_mounted_network_target_id(runtime_store, &server_addr, &remote_disk_id)
        {
            notify_data_changed(target_id);
        }
    }

    changed
}

fn find_mounted_network_target_id(
    runtime_store: &DiskRuntimeStore,
    server_addr: &str,
    remote_disk_id: &str,
) -> Option<u32> {
    runtime_store.runtimes().find_map(|runtime| {
        let Some((runtime_server_addr, runtime_remote_disk_id)) = runtime.network_key() else {
            return None;
        };
        if runtime_server_addr == server_addr && runtime_remote_disk_id == remote_disk_id {
            return runtime.mounted_target_id();
        }
        None
    })
}

#[cfg(test)]
mod tests {
    use std::sync::Arc;
    use std::sync::Mutex;

    use backend_rust::BackendContext;
    use network_core::client::DiskSession;
    use network_core::client::SessionMetadata;
    use network_core::test_support::stage_connection;
    use network_core::transport::TransportEndpoint;

    use super::sync_pending_events;
    use super::sync_pending_events_with_notify;
    use crate::network::NETWORK_SESSION_MISSING_REASON;
    use crate::state::disk_runtime::DiskRuntime;
    use crate::state::disk_runtime::DiskRuntimeStatus;
    use crate::state::disk_runtime::DiskRuntimeStore;
    use crate::state::network_client::NetworkClientEvent;
    use crate::state::network_client::NetworkClientState;
    use crate::state::network_client::NetworkCreateDraft;
    use crate::state::network_client::NetworkDiskKey;
    use crate::state::network_client::NetworkDraftItem;
    use crate::state::network_client::OpenedNetworkDiskSession;

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

    #[test]
    fn session_closed_event_removes_matching_draft_item() {
        let backend = BackendContext::default();
        let mut network_client = NetworkClientState::default();
        let connection = stage_connection(TransportEndpoint::new("127.0.0.1:9004"), 17);
        let session = DiskSession::new(Arc::clone(&connection), 17).expect("session should build");

        let mut draft = NetworkCreateDraft::new(
            "draft-1".to_string(),
            "127.0.0.1:9004".to_string(),
            connection,
        );
        draft.insert_item(NetworkDraftItem {
            key: NetworkDiskKey::new("127.0.0.1:9004", "A1b2C3d4E5f6G7h8"),
            disk_name: "draft-disk".to_string(),
            auth_material: "claim-1".to_string(),
            session,
            metadata: SessionMetadata {
                disk_size_bytes: 4096,
                read_only: false,
                backend_id: [0; 16],
            },
        });
        network_client.insert_draft(draft);
        network_client.push_test_event(NetworkClientEvent::SessionClosed {
            server_addr: "127.0.0.1:9004".to_string(),
            session_id: 17,
        });

        let network_client_mutex = Mutex::new(network_client);
        let mut runtime_store = DiskRuntimeStore::default();

        let changed = sync_pending_events(&backend, &mut runtime_store, &network_client_mutex);
        assert!(changed);

        let network_client = network_client_mutex
            .lock()
            .expect("network client mutex should not be poisoned");
        let draft = network_client
            .draft("draft-1")
            .expect("draft should still exist");
        assert!(draft.items.is_empty());
    }

    #[test]
    fn session_closed_event_invalidates_matching_runtime() {
        let backend = BackendContext::default();
        let mut network_client = NetworkClientState::default();
        let connection = stage_connection(TransportEndpoint::new("127.0.0.1:9006"), 19);
        let session = DiskSession::new(Arc::clone(&connection), 19).expect("session should build");
        let key = NetworkDiskKey::new("127.0.0.1:9006", "L1k2J3h4G5f6D7s8");

        network_client.insert_opened_session(OpenedNetworkDiskSession {
            key: key.clone(),
            session,
            metadata: SessionMetadata {
                disk_size_bytes: 4096,
                read_only: false,
                backend_id: [0; 16],
            },
        });
        network_client.push_test_event(NetworkClientEvent::SessionClosed {
            server_addr: "127.0.0.1:9006".to_string(),
            session_id: 19,
        });

        let network_client_mutex = Mutex::new(network_client);
        let mut runtime_store = DiskRuntimeStore::default();
        let mut runtime = DiskRuntime::new_network(
            "disk-1".to_string(),
            "network-disk".to_string(),
            false,
            "127.0.0.1:9006".to_string(),
            "L1k2J3h4G5f6D7s8".to_string(),
            "claim-3".to_string(),
            4096,
            false,
            false,
        );
        runtime.set_network_unmounted(4096, false);
        runtime_store.insert_runtime(runtime);

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

        let network_client = network_client_mutex
            .lock()
            .expect("network client mutex should not be poisoned");
        assert!(network_client.opened_session(&key).is_none());
    }

    #[test]
    fn connection_lost_event_invalidates_matching_runtime() {
        let backend = BackendContext::default();
        let mut network_client = NetworkClientState::default();
        let connection = stage_connection(TransportEndpoint::new("127.0.0.1:9005"), 21);
        let session = DiskSession::new(Arc::clone(&connection), 21).expect("session should build");

        network_client.insert_opened_session(OpenedNetworkDiskSession {
            key: NetworkDiskKey::new("127.0.0.1:9005", "Z9y8X7w6V5u4T3s2"),
            session,
            metadata: SessionMetadata {
                disk_size_bytes: 4096,
                read_only: false,
                backend_id: [0; 16],
            },
        });
        network_client.push_test_event(NetworkClientEvent::ConnectionLost {
            server_addr: "127.0.0.1:9005".to_string(),
        });

        let network_client_mutex = Mutex::new(network_client);
        let mut runtime_store = DiskRuntimeStore::default();
        let mut runtime = DiskRuntime::new_network(
            "disk-1".to_string(),
            "network-disk".to_string(),
            false,
            "127.0.0.1:9005".to_string(),
            "Z9y8X7w6V5u4T3s2".to_string(),
            "claim-2".to_string(),
            4096,
            false,
            false,
        );
        runtime.set_network_unmounted(4096, false);
        runtime_store.insert_runtime(runtime);

        let changed = sync_pending_events(&backend, &mut runtime_store, &network_client_mutex);
        assert!(changed);

        let runtime = runtime_store
            .find_runtime("disk-1")
            .expect("runtime should exist");
        assert_eq!(
            runtime.status(),
            &DiskRuntimeStatus::Invalid {
                reason: "网络盘连接不可用".to_string(),
            }
        );
    }

    #[test]
    fn connection_lost_event_clears_draft_items_for_server() {
        let backend = BackendContext::default();
        let mut network_client = NetworkClientState::default();
        let connection = stage_connection(TransportEndpoint::new("127.0.0.1:9007"), 23);
        let session = DiskSession::new(Arc::clone(&connection), 23).expect("session should build");

        let mut draft = NetworkCreateDraft::new(
            "draft-1".to_string(),
            "127.0.0.1:9007".to_string(),
            connection,
        );
        draft.insert_item(NetworkDraftItem {
            key: NetworkDiskKey::new("127.0.0.1:9007", "P1o2I3u4Y5t6R7e8"),
            disk_name: "draft-disk".to_string(),
            auth_material: "claim-4".to_string(),
            session,
            metadata: SessionMetadata {
                disk_size_bytes: 4096,
                read_only: false,
                backend_id: [0; 16],
            },
        });
        network_client.insert_draft(draft);
        network_client.push_test_event(NetworkClientEvent::ConnectionLost {
            server_addr: "127.0.0.1:9007".to_string(),
        });

        let network_client_mutex = Mutex::new(network_client);
        let mut runtime_store = DiskRuntimeStore::default();

        let changed = sync_pending_events(&backend, &mut runtime_store, &network_client_mutex);
        assert!(changed);

        let network_client = network_client_mutex
            .lock()
            .expect("network client mutex should not be poisoned");
        let draft = network_client
            .draft("draft-1")
            .expect("draft should still exist");
        assert!(draft.items.is_empty());
    }

    #[test]
    fn data_changed_event_notifies_mounted_runtime_once_per_round() {
        let backend = BackendContext::default();
        let mut network_client = NetworkClientState::default();
        let connection = stage_connection(TransportEndpoint::new("127.0.0.1:9008"), 29);
        let session = DiskSession::new(Arc::clone(&connection), 29).expect("session should build");
        let key = NetworkDiskKey::new("127.0.0.1:9008", "D1a2T3a4C5h6G7e8");

        network_client.insert_opened_session(OpenedNetworkDiskSession {
            key: key.clone(),
            session,
            metadata: SessionMetadata {
                disk_size_bytes: 4096,
                read_only: false,
                backend_id: [0; 16],
            },
        });
        network_client.push_test_event(NetworkClientEvent::SessionDataChanged {
            server_addr: "127.0.0.1:9008".to_string(),
            session_id: 29,
        });
        network_client.push_test_event(NetworkClientEvent::SessionDataChanged {
            server_addr: "127.0.0.1:9008".to_string(),
            session_id: 29,
        });

        let network_client_mutex = Mutex::new(network_client);
        let mut runtime_store = DiskRuntimeStore::default();
        let mut runtime = DiskRuntime::new_network(
            "disk-1".to_string(),
            "network-disk".to_string(),
            false,
            "127.0.0.1:9008".to_string(),
            "D1a2T3a4C5h6G7e8".to_string(),
            "claim-5".to_string(),
            4096,
            false,
            false,
        );
        runtime.set_mounted(7);
        runtime_store.insert_runtime(runtime);

        let mut notified_targets = Vec::new();
        let changed = sync_pending_events_with_notify(
            &backend,
            &mut runtime_store,
            &network_client_mutex,
            |target_id| notified_targets.push(target_id),
        );
        assert!(!changed);
        assert_eq!(notified_targets, vec![7]);

        let runtime = runtime_store
            .find_runtime("disk-1")
            .expect("runtime should exist");
        assert_eq!(
            runtime.status(),
            &DiskRuntimeStatus::Mounted { target_id: 7 }
        );
        let network_client = network_client_mutex
            .lock()
            .expect("network client mutex should not be poisoned");
        assert!(network_client.opened_session(&key).is_some());
    }

    #[test]
    fn data_changed_event_ignores_unmounted_runtime() {
        let backend = BackendContext::default();
        let mut network_client = NetworkClientState::default();
        let connection = stage_connection(TransportEndpoint::new("127.0.0.1:9009"), 31);
        let session = DiskSession::new(Arc::clone(&connection), 31).expect("session should build");

        network_client.insert_opened_session(OpenedNetworkDiskSession {
            key: NetworkDiskKey::new("127.0.0.1:9009", "U1n2M3o4U5n6T7d8"),
            session,
            metadata: SessionMetadata {
                disk_size_bytes: 4096,
                read_only: false,
                backend_id: [0; 16],
            },
        });
        network_client.push_test_event(NetworkClientEvent::SessionDataChanged {
            server_addr: "127.0.0.1:9009".to_string(),
            session_id: 31,
        });

        let network_client_mutex = Mutex::new(network_client);
        let mut runtime_store = DiskRuntimeStore::default();
        let mut runtime = DiskRuntime::new_network(
            "disk-1".to_string(),
            "network-disk".to_string(),
            false,
            "127.0.0.1:9009".to_string(),
            "U1n2M3o4U5n6T7d8".to_string(),
            "claim-6".to_string(),
            4096,
            false,
            false,
        );
        runtime.set_network_unmounted(4096, false);
        runtime_store.insert_runtime(runtime);

        let mut notified_targets = Vec::new();
        let changed = sync_pending_events_with_notify(
            &backend,
            &mut runtime_store,
            &network_client_mutex,
            |target_id| notified_targets.push(target_id),
        );
        assert!(!changed);
        assert!(notified_targets.is_empty());

        let runtime = runtime_store
            .find_runtime("disk-1")
            .expect("runtime should exist");
        assert_eq!(runtime.status(), &DiskRuntimeStatus::Unmounted);
    }
}
