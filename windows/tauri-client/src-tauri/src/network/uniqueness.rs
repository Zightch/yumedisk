use crate::api_error::ApiError;
use crate::state::disk_runtime::DiskRuntimeStore;
use crate::state::network_client::NetworkClientState;
use crate::state::network_client::NetworkDiskKey;

pub fn ensure_unique_network_key(
    runtime_store: &DiskRuntimeStore,
    network_client: &NetworkClientState,
    key: &NetworkDiskKey,
    excluded_draft_id: Option<&str>,
) -> Result<(), ApiError> {
    if runtime_store.runtimes().any(|runtime| {
        runtime.server_addr() == Some(key.server_addr.as_str())
            && runtime.remote_disk_id() == Some(key.remote_disk_id.as_str())
    }) {
        return Err(ApiError::new(
            "network-disk-duplicate",
            "网络盘已存在",
            Some(format!("{} | {}", key.server_addr, key.remote_disk_id)),
        ));
    }

    for draft_id in network_client.draft_ids_for_server(&key.server_addr) {
        if excluded_draft_id.is_some_and(|excluded| excluded == draft_id) {
            continue;
        }

        let is_duplicate = network_client
            .draft(&draft_id)
            .map(|draft| draft.items.contains_key(&key.remote_disk_id))
            .unwrap_or(false);
        if is_duplicate {
            return Err(ApiError::new(
                "network-disk-duplicate",
                "网络盘已存在",
                Some(format!("{} | {}", key.server_addr, key.remote_disk_id)),
            ));
        }
    }

    Ok(())
}

#[cfg(test)]
mod tests {
    use network_core::client::DiskSession;
    use network_core::client::SessionMetadata;
    use network_core::test_support::stage_connection;
    use network_core::transport::TransportEndpoint;

    use super::ensure_unique_network_key;
    use crate::state::disk_runtime::DiskRuntime;
    use crate::state::disk_runtime::DiskRuntimeStore;
    use crate::state::network_client::NetworkClientState;
    use crate::state::network_client::NetworkCreateDraft;
    use crate::state::network_client::NetworkDiskKey;
    use crate::state::network_client::NetworkDraftItem;

    #[test]
    fn duplicate_network_key_is_rejected_when_runtime_exists() {
        let mut runtime_store = DiskRuntimeStore::default();
        runtime_store.insert_runtime(DiskRuntime::new_network(
            "disk-1".to_string(),
            "runtime-disk".to_string(),
            false,
            "127.0.0.1:9000".to_string(),
            "A1b2C3d4E5f6G7h8".to_string(),
            "claim-1".to_string(),
            4096,
            false,
        ));

        let network_client = NetworkClientState::default();
        let error = ensure_unique_network_key(
            &runtime_store,
            &network_client,
            &NetworkDiskKey::new("127.0.0.1:9000", "A1b2C3d4E5f6G7h8"),
            None,
        )
        .expect_err("duplicate runtime key should fail");

        assert_eq!(error.code, "network-disk-duplicate");
    }

    #[test]
    fn duplicate_network_key_is_rejected_when_other_draft_exists() {
        let runtime_store = DiskRuntimeStore::default();
        let connection = stage_connection(TransportEndpoint::new("127.0.0.1:9001"), 17);
        let session = DiskSession::new(connection.clone(), 17).expect("session should build");

        let mut network_client = NetworkClientState::default();
        let mut draft = NetworkCreateDraft::new(
            "draft-1".to_string(),
            "127.0.0.1:9001".to_string(),
            connection,
        );
        draft.insert_item(NetworkDraftItem {
            key: NetworkDiskKey::new("127.0.0.1:9001", "Z9y8X7w6V5u4T3s2"),
            disk_name: "draft-disk".to_string(),
            auth_material: "claim-2".to_string(),
            session,
            metadata: SessionMetadata {
                disk_size_bytes: 4096,
                max_io_bytes: 4096,
                read_only: false,
            },
        });
        network_client.insert_draft(draft);

        let error = ensure_unique_network_key(
            &runtime_store,
            &network_client,
            &NetworkDiskKey::new("127.0.0.1:9001", "Z9y8X7w6V5u4T3s2"),
            Some("draft-2"),
        )
        .expect_err("duplicate draft key should fail");

        assert_eq!(error.code, "network-disk-duplicate");
    }
}
