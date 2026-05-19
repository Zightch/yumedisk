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
