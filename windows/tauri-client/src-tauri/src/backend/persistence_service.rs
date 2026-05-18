use std::collections::BTreeSet;
use std::path::PathBuf;

use backend_rust::BackendContext;
use backend_rust::Media;
use backend_rust::SessionConfig;

use crate::api_error::ApiError;
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
    let mut disk_ids = BTreeSet::new();

    for persisted_disk in persisted_config.disks {
        if !disk_ids.insert(persisted_disk.disk_id.clone()) {
            return Err(ApiError::new(
                "client-config-duplicate-disk-id",
                "配置文件中存在重复磁盘标识",
                Some(persisted_disk.disk_id),
            ));
        }

        let runtime = restore_disk_runtime(persisted_disk)?;
        runtime_store.insert_runtime(runtime);
    }

    Ok(runtime_store)
}

fn restore_disk_runtime(persisted_disk: PersistedDiskRecord) -> Result<DiskRuntime, ApiError> {
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
                persisted_disk.disk_id,
                persisted_disk.disk_name,
                persisted_disk.auto_mount,
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

            match probe_raw_file_media(&file_path) {
                Ok(probe) => Ok(DiskRuntime::new_file(
                    persisted_disk.disk_id,
                    persisted_disk.disk_name,
                    persisted_disk.auto_mount,
                    file_kind,
                    file_path,
                    probe.capacity_bytes,
                    probe.read_only,
                    crate::state::disk_runtime::DiskRuntimeStatus::Unmounted,
                    None,
                )),
                Err(error) => Ok(DiskRuntime::new_file(
                    persisted_disk.disk_id,
                    persisted_disk.disk_name,
                    persisted_disk.auto_mount,
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
    }
}

fn map_session_config(session_config: SessionConfig) -> client_config::PersistedSessionConfig {
    client_config::PersistedSessionConfig {
        heartbeat_interval_ms: session_config.heartbeat_interval_ms,
        initial_event_queue_capacity: session_config.initial_event_queue_capacity,
    }
}

fn map_persisted_disk_record(
    snapshot: crate::state::disk_runtime::DiskRuntimeSnapshot,
) -> PersistedDiskRecord {
    PersistedDiskRecord {
        disk_id: snapshot.disk_id,
        disk_name: snapshot.disk_name,
        auto_mount: snapshot.auto_mount,
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

pub fn probe_raw_file_media(file_path: &str) -> Result<RawFileProbe, ApiError> {
    let file_path_buf = PathBuf::from(file_path);
    if !file_path_buf.is_file() {
        return Err(ApiError::new(
            "invalid-file-path",
            "文件路径必须指向已存在文件",
            Some(file_path.to_string()),
        ));
    }

    let media = RawFileMedia::open(&file_path_buf)
        .map_err(|error| map_raw_file_media_error(error, file_path))?;

    Ok(RawFileProbe {
        capacity_bytes: media.size_bytes(),
        read_only: media.read_only(),
    })
}

pub fn open_raw_file_media(file_path: &str) -> Result<RawFileMedia, ApiError> {
    let file_path_buf = PathBuf::from(file_path);
    if !file_path_buf.is_file() {
        return Err(ApiError::new(
            "invalid-file-path",
            "文件路径必须指向已存在文件",
            Some(file_path.to_string()),
        ));
    }

    RawFileMedia::open(&file_path_buf).map_err(|error| map_raw_file_media_error(error, file_path))
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
            "RAW 文件至少需要一个完整的 512 字节盘块",
            Some(format!("{} | actualBytes={}", file_path, actual_size_bytes)),
        ),
    }
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
