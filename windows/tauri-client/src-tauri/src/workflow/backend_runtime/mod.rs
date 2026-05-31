use backend_rust::BackendContext;
use backend_rust::ManagedDiskEvent;
use backend_rust::ManagedDiskEventType;
use backend_rust::ManagedDiskResponse;
use backend_rust::ManagedDiskResponseType;
use backend_rust::ManagedSessionNotice;
use backend_rust::ManagedSessionNoticeType;
use serde::Serialize;

use crate::backend::persistence_service;
use crate::state::disk_runtime::DiskRuntime;
use crate::state::disk_runtime::DiskRuntimeStore;

const INVALID_FILE_REASON: &str = "文件路径必须指向已存在文件";
const MEMORY_MEDIA_LOST_REASON: &str = "内存盘介质已丢失";

#[derive(Debug, Clone, Default, PartialEq, Eq)]
pub struct BackendRuntimeSyncResult {
    pub runtime_changed: bool,
    pub app_session_failed: Option<String>,
}

#[derive(Debug, Clone, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct AppSessionRuntimeEvent {
    pub phase: String,
    pub status_text: String,
}

pub const APP_SESSION_RUNTIME_CHANGED_EVENT: &str = "app-session-runtime-changed";

pub fn sync_runtime_state(
    backend: &BackendContext,
    runtime_store: &mut DiskRuntimeStore,
) -> BackendRuntimeSyncResult {
    let mut result = BackendRuntimeSyncResult::default();

    drain_disk_responses(backend, runtime_store, &mut result);
    drain_disk_events(backend);
    drain_session_notices(backend, runtime_store, &mut result);

    result
}

fn drain_disk_responses(
    backend: &BackendContext,
    runtime_store: &mut DiskRuntimeStore,
    result: &mut BackendRuntimeSyncResult,
) {
    while let Some(response) = backend.poll_managed_disk_response() {
        result.runtime_changed |= apply_disk_response(runtime_store, &response);
    }
}

fn drain_disk_events(backend: &BackendContext) {
    while let Some(event) = backend.poll_managed_disk_event() {
        apply_disk_event(&event);
    }
}

fn drain_session_notices(
    backend: &BackendContext,
    runtime_store: &mut DiskRuntimeStore,
    result: &mut BackendRuntimeSyncResult,
) {
    let mut broken_status_text = None;

    while let Some(notice) = backend.poll_managed_session_notice() {
        if let Some(status_text) = map_session_notice_failure_text(&notice) {
            broken_status_text = Some(status_text);
        }
    }

    if let Some(status_text) = broken_status_text {
        backend.close();
        result.runtime_changed |= apply_session_broken(runtime_store);
        result.app_session_failed = Some(status_text);
    }
}

fn apply_disk_response(
    runtime_store: &mut DiskRuntimeStore,
    response: &ManagedDiskResponse,
) -> bool {
    match response.response_type {
        ManagedDiskResponseType::DiskRemoved => {
            let Some(runtime) = runtime_store.find_runtime_mut_by_target_id(response.target_id)
            else {
                return false;
            };
            detach_runtime_after_backend_loss(runtime)
        }
        ManagedDiskResponseType::DiskOnline
        | ManagedDiskResponseType::WriteFinalCommitted
        | ManagedDiskResponseType::WriteFinalRejected => false,
    }
}

fn apply_disk_event(event: &ManagedDiskEvent) {
    match event.event_type {
        ManagedDiskEventType::Reserved0 => {}
    }
}

fn map_session_notice_failure_text(notice: &ManagedSessionNotice) -> Option<String> {
    match notice.notice_type {
        ManagedSessionNoticeType::Broken => Some(format!(
            "Backend 会话已失效 ({})",
            format_status_hex(notice.status)
        )),
    }
}

fn apply_session_broken(runtime_store: &mut DiskRuntimeStore) -> bool {
    let mut changed = false;

    for runtime in runtime_store.runtimes_mut() {
        if runtime.mounted_target_id().is_none() {
            continue;
        }

        changed |= detach_runtime_after_backend_loss(runtime);
    }

    changed
}

fn detach_runtime_after_backend_loss(runtime: &mut DiskRuntime) -> bool {
    if runtime.is_network() {
        runtime.set_network_unmounted(runtime.capacity_bytes(), runtime.source_read_only());
        return true;
    }

    if runtime.is_memory() {
        runtime.set_memory_invalid(MEMORY_MEDIA_LOST_REASON.to_string());
        return true;
    }

    let Some(file_path) = runtime.file_path().map(str::to_string) else {
        return false;
    };

    match persistence_service::probe_raw_file_media(&file_path) {
        Ok(probe) => runtime.set_file_unmounted(probe.capacity_bytes, probe.read_only),
        Err(_) => runtime.set_file_invalid(INVALID_FILE_REASON.to_string()),
    }
    true
}

fn format_status_hex(status: i32) -> String {
    format!("0x{:08X}", status as u32)
}

#[cfg(test)]
mod tests {
    use std::fs;
    use std::path::PathBuf;
    use std::time::SystemTime;
    use std::time::UNIX_EPOCH;

    use backend_rust::ManagedDiskResponseType;
    use backend_rust::ManagedDiskEventType;
    use backend_rust::ManagedSessionNoticeType;

    use super::INVALID_FILE_REASON;
    use super::MEMORY_MEDIA_LOST_REASON;
    use super::apply_disk_response;
    use super::apply_disk_event;
    use super::apply_session_broken;
    use super::map_session_notice_failure_text;
    use crate::state::disk_runtime::DiskRuntime;
    use crate::state::disk_runtime::DiskRuntimeStatus;
    use crate::state::disk_runtime::DiskRuntimeStore;
    use crate::state::disk_runtime::FileMediaKind;

    #[test]
    fn disk_removed_response_unmounts_mounted_file_runtime() {
        let file_path = create_temp_raw_file(4096);
        let mut runtime_store = DiskRuntimeStore::default();
        runtime_store.insert_runtime(DiskRuntime::new_file(
            "disk-1".to_string(),
            "file-disk".to_string(),
            false,
            false,
            FileMediaKind::RawFile,
            file_path.display().to_string(),
            4096,
            false,
            DiskRuntimeStatus::Mounted { target_id: 7 },
            None,
        ));

        let changed = apply_disk_response(
            &mut runtime_store,
            &backend_rust::ManagedDiskResponse {
                response_type: ManagedDiskResponseType::DiskRemoved,
                target_id: 7,
                disk_runtime_id: 0,
                event_id: 0,
                total_seq: 0,
                flags: 0,
                status: 0,
            },
        );

        let runtime = runtime_store
            .find_runtime("disk-1")
            .expect("runtime should exist");
        assert!(changed);
        assert_eq!(runtime.status(), &DiskRuntimeStatus::Unmounted);

        let _ = fs::remove_file(file_path);
    }

    #[test]
    fn session_broken_detaches_mounted_runtimes_by_media_kind() {
        let file_path = create_temp_raw_file(4096);
        let mut runtime_store = DiskRuntimeStore::default();
        runtime_store.insert_runtime(DiskRuntime::new_memory(
            "disk-1".to_string(),
            "memory-disk".to_string(),
            false,
            false,
            crate::state::disk_runtime::MemoryMediaKind::DenseMem,
            4096,
            Box::new(crate::backend::memory_media::SparseMemoryMedia::new(4096)),
        ));
        runtime_store
            .find_runtime_mut("disk-1")
            .expect("memory runtime should exist")
            .set_mounted(3);
        runtime_store.insert_runtime(DiskRuntime::new_file(
            "disk-2".to_string(),
            "file-disk".to_string(),
            false,
            false,
            FileMediaKind::RawFile,
            file_path.display().to_string(),
            4096,
            false,
            DiskRuntimeStatus::Mounted { target_id: 5 },
            None,
        ));
        runtime_store.insert_runtime(DiskRuntime::new_network(
            "disk-3".to_string(),
            "network-disk".to_string(),
            false,
            "127.0.0.1:9003".to_string(),
            "A1b2C3d4E5f6G7h8".to_string(),
            "claim-1".to_string(),
            4096,
            false,
            false,
        ));
        runtime_store
            .find_runtime_mut("disk-3")
            .expect("network runtime should exist")
            .set_mounted(9);

        let changed = apply_session_broken(&mut runtime_store);

        let memory_runtime = runtime_store
            .find_runtime("disk-1")
            .expect("memory runtime should exist");
        let file_runtime = runtime_store
            .find_runtime("disk-2")
            .expect("file runtime should exist");
        let network_runtime = runtime_store
            .find_runtime("disk-3")
            .expect("network runtime should exist");

        assert!(changed);
        assert_eq!(
            memory_runtime.status(),
            &DiskRuntimeStatus::Invalid {
                reason: MEMORY_MEDIA_LOST_REASON.to_string(),
            }
        );
        assert_eq!(file_runtime.status(), &DiskRuntimeStatus::Unmounted);
        assert_eq!(network_runtime.status(), &DiskRuntimeStatus::Unmounted);

        let _ = fs::remove_file(file_path);
    }

    #[test]
    fn session_notice_failure_text_uses_status_hex() {
        let text = map_session_notice_failure_text(&backend_rust::ManagedSessionNotice {
            notice_type: ManagedSessionNoticeType::Broken,
            flags: 0,
            status: -1073741823,
        })
        .expect("broken notice should map to failure text");

        assert_eq!(text, "Backend 会话已失效 (0xC0000001)");
    }

    #[test]
    fn disk_removed_response_marks_missing_file_invalid() {
        let missing_path = unique_temp_path("missing-runtime.raw");
        let mut runtime_store = DiskRuntimeStore::default();
        runtime_store.insert_runtime(DiskRuntime::new_file(
            "disk-1".to_string(),
            "file-disk".to_string(),
            false,
            false,
            FileMediaKind::RawFile,
            missing_path.display().to_string(),
            4096,
            false,
            DiskRuntimeStatus::Mounted { target_id: 11 },
            None,
        ));

        let changed = apply_disk_response(
            &mut runtime_store,
            &backend_rust::ManagedDiskResponse {
                response_type: ManagedDiskResponseType::DiskRemoved,
                target_id: 11,
                disk_runtime_id: 0,
                event_id: 0,
                total_seq: 0,
                flags: 0,
                status: 0,
            },
        );

        let runtime = runtime_store
            .find_runtime("disk-1")
            .expect("runtime should exist");
        assert!(changed);
        assert_eq!(
            runtime.status(),
            &DiskRuntimeStatus::Invalid {
                reason: INVALID_FILE_REASON.to_string(),
            }
        );
    }

    #[test]
    fn reserved_disk_event_is_noop() {
        apply_disk_event(&backend_rust::ManagedDiskEvent {
            event_type: ManagedDiskEventType::Reserved0,
            target_id: 17,
            disk_runtime_id: 19,
            flags: 0,
            status: 0,
        });
    }

    fn create_temp_raw_file(size_bytes: u64) -> PathBuf {
        let path = unique_temp_path("backend-runtime.raw");
        let file = std::fs::OpenOptions::new()
            .write(true)
            .create_new(true)
            .open(&path)
            .expect("temp raw file should create");
        file.set_len(size_bytes)
            .expect("temp raw file size should be set");
        path
    }

    fn unique_temp_path(file_name: &str) -> PathBuf {
        let nonce = SystemTime::now()
            .duration_since(UNIX_EPOCH)
            .expect("system time should be after unix epoch")
            .as_nanos();
        std::env::temp_dir().join(format!("yumedisk-{}-{}", nonce, file_name))
    }
}
