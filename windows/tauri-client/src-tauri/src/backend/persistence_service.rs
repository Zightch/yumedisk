use std::collections::BTreeSet;
use std::path::Component;
use std::path::Path;
use std::path::PathBuf;

use backend_rust::BackendContext;
use backend_rust::Media;
use backend_rust::SessionConfig;

use crate::api_error::ApiError;
use crate::backend::config_service;
use crate::backend::memory_media::DenseMemoryMedia;
use crate::backend::memory_media::DenseMemoryMediaError;
use crate::backend::memory_media::SparseMemoryMedia;
use crate::backend::raw_file_media::RawFileMedia;
use crate::backend::raw_file_media::RawFileMediaError;
use crate::state::client_config;
use crate::state::client_config::ClientConfigStoreError;
use crate::state::client_config::PersistedClientConfig;
use crate::state::client_config::PersistedDiskMediaConfig;
use crate::state::client_config::PersistedDiskRecord;
use crate::state::client_config::PersistedFileMediaKind;
use crate::state::client_config::PersistedMemoryMediaKind;
use crate::state::disk_runtime::DiskRuntime;
use crate::state::disk_runtime::DiskRuntimeStore;
use crate::state::disk_runtime::FileMediaKind;
use crate::state::disk_runtime::MemoryMediaKind;

const INVALID_FILE_REASON: &str = "文件路径必须指向已存在文件";
pub(crate) const DUPLICATE_FILE_REASON: &str = "文件路径重复";

pub struct RestoredClientState {
    pub session_config: SessionConfig,
    pub disk_runtime_store: DiskRuntimeStore,
}

pub fn load_client_state() -> Result<RestoredClientState, ApiError> {
    let persisted_config = client_config::load_client_config().map_err(map_client_config_error)?;
    let session_config = SessionConfig {
        heartbeat_interval_ms: persisted_config.session_config.heartbeat_interval_ms,
        initial_event_queue_capacity: persisted_config.session_config.initial_event_queue_capacity,
    };
    let disk_runtime_store = build_restored_runtime_store(persisted_config)?;

    Ok(RestoredClientState {
        session_config,
        disk_runtime_store,
    })
}

pub fn save_client_state(
    backend: &BackendContext,
    runtime_store: &DiskRuntimeStore,
) -> Result<(), ApiError> {
    let persisted_config = PersistedClientConfig {
        version: client_config::CONFIG_VERSION,
        session_config: map_session_config(backend.session_config()),
        disks: runtime_store
            .snapshots()
            .into_iter()
            .map(map_persisted_disk_record)
            .collect(),
    };

    client_config::save_client_config(&persisted_config).map_err(map_client_config_error)
}

fn build_restored_runtime_store(
    persisted_config: PersistedClientConfig,
) -> Result<DiskRuntimeStore, ApiError> {
    let mut runtime_store = DiskRuntimeStore::default();
    let mut local_disk_ids = BTreeSet::new();
    let mut restored_file_paths = BTreeSet::new();

    for persisted_disk in persisted_config.disks {
        if !local_disk_ids.insert(persisted_disk.local_disk_id.clone()) {
            return Err(ApiError::new(
                "client-config-duplicate-disk-id",
                "配置文件中存在重复磁盘标识",
                Some(persisted_disk.local_disk_id),
            ));
        }

        let runtime = restore_disk_runtime(persisted_disk, &mut restored_file_paths)?;
        runtime_store.insert_runtime(runtime);
    }

    Ok(runtime_store)
}

fn restore_disk_runtime(
    persisted_disk: PersistedDiskRecord,
    restored_file_paths: &mut BTreeSet<String>,
) -> Result<DiskRuntime, ApiError> {
    match persisted_disk.media {
        PersistedDiskMediaConfig::Memory {
            memory_kind,
            capacity_bytes,
        } => {
            let memory_kind = match memory_kind {
                PersistedMemoryMediaKind::DenseMem => MemoryMediaKind::DenseMem,
                PersistedMemoryMediaKind::SparseMem => MemoryMediaKind::SparseMem,
            };
            let media = create_memory_media(memory_kind, capacity_bytes)?;

            Ok(DiskRuntime::new_memory(
                persisted_disk.local_disk_id,
                persisted_disk.disk_name,
                persisted_disk.auto_mount,
                persisted_disk.configured_read_only,
                memory_kind,
                capacity_bytes,
                media,
            ))
        }
        PersistedDiskMediaConfig::File {
            file_kind,
            file_path,
        } => {
            let file_kind = match file_kind {
                PersistedFileMediaKind::RawFile => FileMediaKind::RawFile,
            };
            let normalized_file_path = match normalize_full_file_path(&file_path) {
                Ok(value) => value,
                Err(error) => {
                    return Ok(DiskRuntime::new_file(
                        persisted_disk.local_disk_id,
                        persisted_disk.disk_name,
                        persisted_disk.auto_mount,
                        persisted_disk.configured_read_only,
                        file_kind,
                        file_path,
                        0,
                        false,
                        crate::state::disk_runtime::DiskRuntimeStatus::Invalid {
                            reason: error.message,
                        },
                        None,
                    ));
                }
            };

            if !restored_file_paths.insert(normalized_file_path) {
                return Ok(DiskRuntime::new_file(
                    persisted_disk.local_disk_id,
                    persisted_disk.disk_name,
                    persisted_disk.auto_mount,
                    persisted_disk.configured_read_only,
                    file_kind,
                    file_path,
                    0,
                    false,
                    crate::state::disk_runtime::DiskRuntimeStatus::Invalid {
                        reason: DUPLICATE_FILE_REASON.to_string(),
                    },
                    None,
                ));
            }

            match probe_raw_file_media(&file_path) {
                Ok(probe) => Ok(DiskRuntime::new_file(
                    persisted_disk.local_disk_id,
                    persisted_disk.disk_name,
                    persisted_disk.auto_mount,
                    persisted_disk.configured_read_only,
                    file_kind,
                    file_path,
                    probe.capacity_bytes,
                    probe.read_only,
                    crate::state::disk_runtime::DiskRuntimeStatus::Unmounted,
                    None,
                )),
                Err(error) => Ok(DiskRuntime::new_file(
                    persisted_disk.local_disk_id,
                    persisted_disk.disk_name,
                    persisted_disk.auto_mount,
                    persisted_disk.configured_read_only,
                    file_kind,
                    file_path,
                    0,
                    false,
                    crate::state::disk_runtime::DiskRuntimeStatus::Invalid {
                        reason: if error.code == "invalid-file-path" {
                            INVALID_FILE_REASON.to_string()
                        } else {
                            error.message
                        },
                    },
                    None,
                )),
            }
        }
        PersistedDiskMediaConfig::Network {
            server_addr,
            remote_disk_id,
            auth_material,
            capacity_bytes,
            source_read_only,
        } => Ok(DiskRuntime::new_network(
            persisted_disk.local_disk_id,
            persisted_disk.disk_name,
            persisted_disk.auto_mount,
            server_addr,
            remote_disk_id,
            auth_material,
            capacity_bytes,
            persisted_disk.configured_read_only,
            source_read_only,
        )),
    }
}

fn map_session_config(session_config: SessionConfig) -> client_config::PersistedAppSessionConfig {
    client_config::PersistedAppSessionConfig {
        heartbeat_interval_ms: session_config.heartbeat_interval_ms,
        initial_event_queue_capacity: session_config.initial_event_queue_capacity,
    }
}

fn map_persisted_disk_record(
    snapshot: crate::state::disk_runtime::DiskRuntimeSnapshot,
) -> PersistedDiskRecord {
    PersistedDiskRecord {
        local_disk_id: snapshot.local_disk_id,
        disk_name: snapshot.disk_name,
        auto_mount: snapshot.auto_mount,
        configured_read_only: snapshot.configured_read_only,
        media: match snapshot.media {
            crate::state::disk_runtime::DiskMediaConfig::Memory {
                memory_kind,
                capacity_bytes,
            } => PersistedDiskMediaConfig::Memory {
                memory_kind: match memory_kind {
                    MemoryMediaKind::DenseMem => PersistedMemoryMediaKind::DenseMem,
                    MemoryMediaKind::SparseMem => PersistedMemoryMediaKind::SparseMem,
                },
                capacity_bytes,
            },
            crate::state::disk_runtime::DiskMediaConfig::File {
                file_kind,
                file_path,
                ..
            } => PersistedDiskMediaConfig::File {
                file_kind: match file_kind {
                    FileMediaKind::RawFile => PersistedFileMediaKind::RawFile,
                },
                file_path,
            },
            crate::state::disk_runtime::DiskMediaConfig::Network {
                server_addr,
                remote_disk_id,
                auth_material,
                capacity_bytes,
            } => PersistedDiskMediaConfig::Network {
                server_addr,
                remote_disk_id,
                auth_material,
                capacity_bytes,
                source_read_only: snapshot.source_read_only,
            },
        },
    }
}

fn create_memory_media(
    memory_kind: MemoryMediaKind,
    capacity_bytes: u64,
) -> Result<Box<dyn Media>, ApiError> {
    match memory_kind {
        MemoryMediaKind::DenseMem => DenseMemoryMedia::new(capacity_bytes)
            .map(|media| Box::new(media) as Box<dyn Media>)
            .map_err(|error| map_dense_memory_media_error(error, capacity_bytes)),
        MemoryMediaKind::SparseMem => Ok(Box::new(SparseMemoryMedia::new(capacity_bytes))),
    }
}

pub struct RawFileProbe {
    pub capacity_bytes: u64,
    pub read_only: bool,
}

pub(crate) fn normalize_full_file_path(file_path: &str) -> Result<String, ApiError> {
    let trimmed = file_path.trim();
    if trimmed.is_empty() {
        return Err(ApiError::new("invalid-file-path", "文件路径不能为空", None));
    }

    let raw_path = PathBuf::from(trimmed);
    let absolute_path = if raw_path.is_absolute() {
        raw_path
    } else {
        std::env::current_dir()
            .map_err(|io_error| {
                ApiError::new(
                    "invalid-file-path",
                    "无法解析文件路径",
                    Some(io_error.to_string()),
                )
            })?
            .join(raw_path)
    };

    let normalized_path = normalize_path_components(&absolute_path);
    let normalized_text = normalized_path.display().to_string();

    if cfg!(windows) {
        Ok(normalized_text.to_ascii_lowercase())
    } else {
        Ok(normalized_text)
    }
}

pub fn probe_raw_file_media(file_path: &str) -> Result<RawFileProbe, ApiError> {
    let file_path_buf = PathBuf::from(file_path);
    if !file_path_buf.is_file() {
        return Err(ApiError::new(
            "invalid-file-path",
            "文件路径必须指向已存在文件",
            Some(file_path.to_string()),
        ));
    }

    let media = RawFileMedia::open(
        &file_path_buf,
        false,
        config_service::query_disk_sector_size_bytes(),
    )
    .map_err(|error| map_raw_file_media_error(error, file_path))?;

    Ok(RawFileProbe {
        capacity_bytes: media.size_bytes(),
        read_only: media.read_only(),
    })
}

pub fn open_raw_file_media(
    file_path: &str,
    force_read_only: bool,
) -> Result<RawFileMedia, ApiError> {
    let file_path_buf = PathBuf::from(file_path);
    if !file_path_buf.is_file() {
        return Err(ApiError::new(
            "invalid-file-path",
            "文件路径必须指向已存在文件",
            Some(file_path.to_string()),
        ));
    }

    RawFileMedia::open(
        &file_path_buf,
        force_read_only,
        config_service::query_disk_sector_size_bytes(),
    )
    .map_err(|error| map_raw_file_media_error(error, file_path))
}

fn map_dense_memory_media_error(error: DenseMemoryMediaError, capacity_bytes: u64) -> ApiError {
    match error {
        DenseMemoryMediaError::SizeExceedsProcessLimit => ApiError::new(
            "dense-memory-size-exceeds-process-limit",
            "denseMem 大小超出当前进程可分配范围",
            Some(capacity_bytes.to_string()),
        ),
        DenseMemoryMediaError::AllocationFailed => ApiError::new(
            "dense-memory-allocation-failed",
            "denseMem 分配失败",
            Some(capacity_bytes.to_string()),
        ),
    }
}

fn map_raw_file_media_error(error: RawFileMediaError, file_path: &str) -> ApiError {
    match error {
        RawFileMediaError::OpenFailed {
            read_write,
            read_only,
        } => ApiError::new(
            "raw-file-open-failed",
            "无法打开 RAW 文件",
            Some(format!(
                "{} | readWrite={} | readOnly={}",
                file_path, read_write, read_only
            )),
        ),
        RawFileMediaError::MetadataFailed(io_error) => ApiError::new(
            "raw-file-metadata-failed",
            "读取 RAW 文件信息失败",
            Some(format!("{} | {}", file_path, io_error)),
        ),
        RawFileMediaError::NoUsableCapacity { actual_size_bytes } => ApiError::new(
            "raw-file-no-usable-capacity",
            "RAW 文件不能为空",
            Some(format!("{} | actualBytes={}", file_path, actual_size_bytes)),
        ),
    }
}

fn normalize_path_components(path: &Path) -> PathBuf {
    let mut normalized = PathBuf::new();

    for component in path.components() {
        match component {
            Component::Prefix(prefix) => normalized.push(prefix.as_os_str()),
            Component::RootDir => normalized.push(component.as_os_str()),
            Component::CurDir => {}
            Component::ParentDir => {
                let _ = normalized.pop();
            }
            Component::Normal(segment) => normalized.push(segment),
        }
    }

    normalized
}

fn map_client_config_error(error: ClientConfigStoreError) -> ApiError {
    match error {
        ClientConfigStoreError::HomeDirectoryNotFound => ApiError::new(
            "client-config-home-directory-not-found",
            "无法定位用户家目录",
            None,
        ),
        ClientConfigStoreError::UnsupportedVersion(version) => ApiError::new(
            "client-config-unsupported-version",
            "配置文件版本不受支持",
            Some(version.to_string()),
        ),
        ClientConfigStoreError::CreateDirectoryFailed { path, source } => ApiError::new(
            "client-config-create-directory-failed",
            "创建配置目录失败",
            Some(format!("{} | {}", path.display(), source)),
        ),
        ClientConfigStoreError::ReadFailed { path, source } => ApiError::new(
            "client-config-read-failed",
            "读取配置文件失败",
            Some(format!("{} | {}", path.display(), source)),
        ),
        ClientConfigStoreError::WriteFailed { path, source } => ApiError::new(
            "client-config-write-failed",
            "写入配置文件失败",
            Some(format!("{} | {}", path.display(), source)),
        ),
        ClientConfigStoreError::ParseFailed { path, source } => ApiError::new(
            "client-config-parse-failed",
            "解析配置文件失败",
            Some(format!("{} | {}", path.display(), source)),
        ),
        ClientConfigStoreError::SerializeFailed(source) => ApiError::new(
            "client-config-serialize-failed",
            "序列化配置文件失败",
            Some(source.to_string()),
        ),
    }
}

#[cfg(test)]
mod tests {
    use std::fs;
    use std::path::PathBuf;
    use std::time::SystemTime;
    use std::time::UNIX_EPOCH;

    use crate::state::client_config::PersistedAppSessionConfig;
    use crate::state::client_config::PersistedClientConfig;
    use crate::state::client_config::PersistedDiskMediaConfig;
    use crate::state::client_config::PersistedDiskRecord;
    use crate::state::client_config::PersistedFileMediaKind;
    use crate::state::disk_runtime::DiskRuntimeStatus;

    use super::build_restored_runtime_store;
    use super::normalize_full_file_path;
    use super::DUPLICATE_FILE_REASON;

    #[test]
    fn normalize_full_file_path_collapses_relative_segments() {
        let current_dir = std::env::current_dir().expect("current dir should exist");
        let expected = current_dir.join("alpha").join("beta.img");
        let input = current_dir
            .join("alpha")
            .join(".")
            .join("gamma")
            .join("..")
            .join("beta.img");

        let normalized = normalize_full_file_path(&input.display().to_string())
            .expect("path normalization should succeed");

        assert_eq!(
            normalized,
            normalize_full_file_path(&expected.display().to_string())
                .expect("expected path should normalize"),
        );
    }

    #[test]
    fn restore_marks_duplicate_file_path_runtime_invalid() {
        let file_path = unique_test_file_path("restore_duplicate_file_path_runtime_invalid.img");
        fs::write(&file_path, vec![0u8; 4096]).expect("test file should be created");

        let normalized = normalize_full_file_path(&file_path.display().to_string())
            .expect("path normalization should succeed");
        let duplicate_path = if cfg!(windows) {
            normalized.to_ascii_uppercase()
        } else {
            normalized.clone()
        };
        let config = PersistedClientConfig {
            version: crate::state::client_config::CONFIG_VERSION,
            session_config: PersistedAppSessionConfig {
                heartbeat_interval_ms: 1000,
                initial_event_queue_capacity: 16,
            },
            disks: vec![
                PersistedDiskRecord {
                    local_disk_id: "disk-1".to_string(),
                    disk_name: "primary".to_string(),
                    auto_mount: false,
                    configured_read_only: false,
                    media: PersistedDiskMediaConfig::File {
                        file_kind: PersistedFileMediaKind::RawFile,
                        file_path: file_path.display().to_string(),
                    },
                },
                PersistedDiskRecord {
                    local_disk_id: "disk-2".to_string(),
                    disk_name: "duplicate".to_string(),
                    auto_mount: false,
                    configured_read_only: false,
                    media: PersistedDiskMediaConfig::File {
                        file_kind: PersistedFileMediaKind::RawFile,
                        file_path: duplicate_path,
                    },
                },
            ],
        };

        let runtime_store =
            build_restored_runtime_store(config).expect("restore should succeed with invalid loser");
        let snapshots = runtime_store.snapshots();

        assert_eq!(snapshots.len(), 2);
        assert_eq!(snapshots[0].status, DiskRuntimeStatus::Unmounted);
        assert_eq!(
            snapshots[1].status,
            DiskRuntimeStatus::Invalid {
                reason: DUPLICATE_FILE_REASON.to_string(),
            },
        );

        let _ = fs::remove_file(file_path);
    }

    fn unique_test_file_path(file_name: &str) -> PathBuf {
        let unique = SystemTime::now()
            .duration_since(UNIX_EPOCH)
            .expect("system time should be valid")
            .as_nanos();
        std::env::temp_dir().join(format!("{}_{}", unique, file_name))
    }
}
