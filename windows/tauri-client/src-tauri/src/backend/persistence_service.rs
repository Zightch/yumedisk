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
use crate::state::disk_store::ConfigDiskRecord;
use crate::state::disk_store::DiskMediaConfig;
use crate::state::disk_store::DiskStore;
use crate::state::disk_store::FileMediaKind;
use crate::state::disk_store::MemoryMediaKind;

pub struct RestoredClientState {
    pub session_config: SessionConfig,
    pub disk_store: DiskStore,
}

pub fn load_client_state() -> Result<RestoredClientState, ApiError> {
    let persisted_config = client_config::load_client_config().map_err(map_client_config_error)?;
    let session_config = SessionConfig {
        heartbeat_interval_ms: persisted_config.session_config.heartbeat_interval_ms,
        initial_event_queue_capacity: persisted_config.session_config.initial_event_queue_capacity,
    };
    let disk_store = build_restored_disk_store(persisted_config)?;

    Ok(RestoredClientState {
        session_config,
        disk_store,
    })
}

pub fn save_client_state(backend: &BackendContext, disk_store: &DiskStore) -> Result<(), ApiError> {
    let persisted_config = PersistedClientConfig {
        version: 1,
        session_config: map_session_config(backend.session_config()),
        disks: disk_store
            .config_disks_snapshot()
            .into_iter()
            .map(map_persisted_disk_record)
            .collect(),
    };

    client_config::save_client_config(&persisted_config).map_err(map_client_config_error)
}

fn build_restored_disk_store(
    persisted_config: PersistedClientConfig,
) -> Result<DiskStore, ApiError> {
    let mut disk_store = DiskStore::default();
    let mut disk_ids = BTreeSet::new();

    for persisted_disk in persisted_config.disks {
        if !disk_ids.insert(persisted_disk.disk_id.clone()) {
            return Err(ApiError::new(
                "client-config-duplicate-disk-id",
                "配置文件中存在重复磁盘标识",
                Some(persisted_disk.disk_id),
            ));
        }

        let (config_disk, media) = restore_disk_record(persisted_disk)?;
        disk_store.insert_unconnected_disk(config_disk, media);
    }

    Ok(disk_store)
}

fn restore_disk_record(
    persisted_disk: PersistedDiskRecord,
) -> Result<(ConfigDiskRecord, Box<dyn Media>), ApiError> {
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

            Ok((
                ConfigDiskRecord {
                    disk_id: persisted_disk.disk_id,
                    disk_name: persisted_disk.disk_name,
                    auto_connect: persisted_disk.auto_connect,
                    read_only: false,
                    media: DiskMediaConfig::Memory {
                        memory_kind,
                        capacity_bytes,
                    },
                },
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
            let raw_file_media = open_raw_file_media(&file_path)?;
            let capacity_bytes = raw_file_media.size_bytes();
            let read_only = raw_file_media.read_only();

            Ok((
                ConfigDiskRecord {
                    disk_id: persisted_disk.disk_id,
                    disk_name: persisted_disk.disk_name,
                    auto_connect: persisted_disk.auto_connect,
                    read_only,
                    media: DiskMediaConfig::File {
                        file_kind,
                        file_path,
                        capacity_bytes,
                    },
                },
                Box::new(raw_file_media),
            ))
        }
    }
}

fn map_session_config(session_config: SessionConfig) -> client_config::PersistedSessionConfig {
    client_config::PersistedSessionConfig {
        heartbeat_interval_ms: session_config.heartbeat_interval_ms,
        initial_event_queue_capacity: session_config.initial_event_queue_capacity,
    }
}

fn map_persisted_disk_record(config_disk: ConfigDiskRecord) -> PersistedDiskRecord {
    PersistedDiskRecord {
        disk_id: config_disk.disk_id,
        disk_name: config_disk.disk_name,
        auto_connect: config_disk.auto_connect,
        media: match config_disk.media {
            DiskMediaConfig::Memory {
                memory_kind,
                capacity_bytes,
            } => PersistedDiskMediaConfig::Memory {
                memory_kind: match memory_kind {
                    MemoryMediaKind::DenseMem => PersistedMemoryMediaKind::DenseMem,
                    MemoryMediaKind::SparseMem => PersistedMemoryMediaKind::SparseMem,
                },
                capacity_bytes,
            },
            DiskMediaConfig::File {
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

fn open_raw_file_media(file_path: &str) -> Result<RawFileMedia, ApiError> {
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
        RawFileMediaError::EmptyFile => ApiError::new(
            "raw-file-empty",
            "RAW 文件大小必须大于 0",
            Some(file_path.to_string()),
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
