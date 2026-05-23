use std::convert::TryFrom;
use std::env;
use std::fmt;
use std::fs;
use std::io;
use std::path::PathBuf;

use serde::Deserialize;
use serde::Serialize;
use serde_json::Value;

pub const CONFIG_VERSION: u32 = 3;
const CONFIG_DIR_NAME: &str = ".yumedisk";
const CONFIG_FILE_NAME: &str = "client.json";

#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub enum PersistedMemoryMediaKind {
    DenseMem,
    SparseMem,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub enum PersistedFileMediaKind {
    RawFile,
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
#[serde(tag = "kind", rename_all = "camelCase")]
pub enum PersistedDiskMediaConfig {
    Memory {
        #[serde(rename = "memoryKind")]
        memory_kind: PersistedMemoryMediaKind,
        #[serde(rename = "capacityBytes")]
        capacity_bytes: u64,
    },
    File {
        #[serde(rename = "fileKind")]
        file_kind: PersistedFileMediaKind,
        #[serde(rename = "filePath")]
        file_path: String,
    },
    Network {
        #[serde(rename = "serverAddr")]
        server_addr: String,
        #[serde(rename = "remoteDiskId")]
        remote_disk_id: String,
        #[serde(rename = "authMaterial")]
        auth_material: String,
        #[serde(rename = "capacityBytes")]
        capacity_bytes: u64,
        #[serde(rename = "sourceReadOnly")]
        source_read_only: bool,
    },
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct PersistedDiskRecord {
    pub local_disk_id: String,
    pub disk_name: String,
    pub auto_mount: bool,
    pub configured_read_only: bool,
    pub media: PersistedDiskMediaConfig,
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct PersistedAppSessionConfig {
    pub heartbeat_interval_ms: u32,
    pub initial_event_queue_capacity: u32,
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct PersistedClientConfig {
    pub version: u32,
    pub session_config: PersistedAppSessionConfig,
    pub disks: Vec<PersistedDiskRecord>,
}

impl Default for PersistedClientConfig {
    fn default() -> Self {
        Self {
            version: CONFIG_VERSION,
            session_config: PersistedAppSessionConfig {
                heartbeat_interval_ms: backend_rust::SessionConfig::default().heartbeat_interval_ms,
                initial_event_queue_capacity: backend_rust::SessionConfig::default()
                    .initial_event_queue_capacity,
            },
            disks: Vec::new(),
        }
    }
}

#[derive(Debug)]
pub enum ClientConfigStoreError {
    HomeDirectoryNotFound,
    UnsupportedVersion(u32),
    CreateDirectoryFailed {
        path: PathBuf,
        source: io::Error,
    },
    ReadFailed {
        path: PathBuf,
        source: io::Error,
    },
    WriteFailed {
        path: PathBuf,
        source: io::Error,
    },
    ParseFailed {
        path: PathBuf,
        source: serde_json::Error,
    },
    SerializeFailed(serde_json::Error),
}

impl fmt::Display for ClientConfigStoreError {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            Self::HomeDirectoryNotFound => write!(formatter, "home directory not found"),
            Self::UnsupportedVersion(version) => {
                write!(formatter, "unsupported config version: {}", version)
            }
            Self::CreateDirectoryFailed { path, source } => {
                write!(
                    formatter,
                    "create directory failed: {} ({})",
                    path.display(),
                    source
                )
            }
            Self::ReadFailed { path, source } => {
                write!(
                    formatter,
                    "read config failed: {} ({})",
                    path.display(),
                    source
                )
            }
            Self::WriteFailed { path, source } => {
                write!(
                    formatter,
                    "write config failed: {} ({})",
                    path.display(),
                    source
                )
            }
            Self::ParseFailed { path, source } => {
                write!(
                    formatter,
                    "parse config failed: {} ({})",
                    path.display(),
                    source
                )
            }
            Self::SerializeFailed(source) => {
                write!(formatter, "serialize config failed: {}", source)
            }
        }
    }
}

pub fn load_client_config() -> Result<PersistedClientConfig, ClientConfigStoreError> {
    let path = resolve_client_config_path()?;
    if !path.is_file() {
        return Ok(PersistedClientConfig::default());
    }

    let text = fs::read_to_string(&path).map_err(|source| ClientConfigStoreError::ReadFailed {
        path: path.clone(),
        source,
    })?;
    let config_value: Value =
        serde_json::from_str(&text).map_err(|source| ClientConfigStoreError::ParseFailed {
            path: path.clone(),
            source,
        })?;
    let version = config_value
        .get("version")
        .and_then(Value::as_u64)
        .and_then(|value| u32::try_from(value).ok())
        .unwrap_or(0);

    if version != CONFIG_VERSION {
        return Err(ClientConfigStoreError::UnsupportedVersion(version));
    }

    let config: PersistedClientConfig = serde_json::from_value(config_value).map_err(|source| {
        ClientConfigStoreError::ParseFailed {
            path: path.clone(),
            source,
        }
    })?;

    Ok(config)
}

pub fn save_client_config(config: &PersistedClientConfig) -> Result<(), ClientConfigStoreError> {
    let path = resolve_client_config_path()?;
    if let Some(parent) = path.parent() {
        fs::create_dir_all(parent).map_err(|source| {
            ClientConfigStoreError::CreateDirectoryFailed {
                path: parent.to_path_buf(),
                source,
            }
        })?;
    }

    let text =
        serde_json::to_string_pretty(config).map_err(ClientConfigStoreError::SerializeFailed)?;
    fs::write(&path, text).map_err(|source| ClientConfigStoreError::WriteFailed {
        path: path.clone(),
        source,
    })?;
    Ok(())
}

pub fn resolve_client_config_path() -> Result<PathBuf, ClientConfigStoreError> {
    let home_directory = env::var_os("USERPROFILE")
        .or_else(|| env::var_os("HOME"))
        .map(PathBuf::from)
        .ok_or(ClientConfigStoreError::HomeDirectoryNotFound)?;

    Ok(home_directory.join(CONFIG_DIR_NAME).join(CONFIG_FILE_NAME))
}
