use std::fmt;
use std::fs;
use std::path::PathBuf;
use std::sync::{Arc, Mutex};
use std::time::Duration;

use backend_rust::BackendError;
use backend_rust::Media;
use cache::{AtIo, Cache, CacheConfig, CacheError, RightIoErrorKind};
use network_core::client::DiskSession;
use network_core::client::NetworkClientError;
use network_core::client::SessionMetadata;
use network_core::protocol::MAX_DATA_PLANE_RAW_BYTES;

const CACHE_BLOCK_SIZE_BYTES: u32 = 32 * 1024;
const CACHE_FIFO_CAPACITY_BLOCKS: usize = 32;
const CACHE_LRU_CAPACITY_BLOCKS: usize = 64;
const CACHE_DIRTY_SCAN_INTERVAL: Duration = Duration::from_millis(50);
const CACHE_TEMP_MAX_FILES: usize = 64;
const CACHE_QUIESCE_TIMEOUT: Duration = Duration::from_secs(5);
const CACHE_TEMP_ROOT_NAME: &str = "yumedisk-network-media";

pub struct NetworkMedia {
    remote_disk_id: String,
    disk_size_bytes: u64,
    read_only: bool,
    invalidation: Arc<InvalidationDispatch>,
    path: Option<MediaPath>,
}

enum MediaPath {
    BypassRo {
        session: DiskSession,
    },
    CachedRw {
        session: DiskSession,
        cache: Cache<DiskSessionAtIo>,
        temp_dir: PathBuf,
    },
}

#[derive(Clone)]
struct DiskSessionAtIo {
    session: DiskSession,
    disk_size_bytes: u64,
    logical_size_bytes: u64,
    block_size_bytes: u32,
    invalidation: Arc<InvalidationDispatch>,
}

#[derive(Default)]
struct InvalidationDispatch {
    handler: Mutex<Option<Arc<dyn Fn() + Send + Sync>>>,
}

impl fmt::Debug for NetworkMedia {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        formatter
            .debug_struct("NetworkMedia")
            .field("remote_disk_id", &self.remote_disk_id)
            .field("session_id", &self.session().session_id())
            .field("disk_size_bytes", &self.disk_size_bytes)
            .field("read_only", &self.read_only)
            .finish()
    }
}

impl NetworkMedia {
    pub fn bind(
        remote_disk_id: impl Into<String>,
        session: DiskSession,
        metadata: SessionMetadata,
        configured_read_only: bool,
    ) -> Result<Self, NetworkClientError> {
        let remote_disk_id = remote_disk_id.into();
        if remote_disk_id.is_empty() {
            return Err(NetworkClientError::InvalidArgument("remote_disk_id"));
        }
        if metadata.disk_size_bytes == 0 {
            return Err(NetworkClientError::InvalidArgument("disk_size_bytes"));
        }

        let invalidation = Arc::new(InvalidationDispatch::default());
        let read_only = configured_read_only || metadata.read_only;
        let path = if read_only {
            MediaPath::BypassRo { session }
        } else {
            let temp_dir = prepare_cache_temp_dir(&session, &remote_disk_id)
                .map_err(|_| NetworkClientError::InvalidState("cache_temp_dir"))?;
            let config = default_cache_config(temp_dir.clone());
            let right = DiskSessionAtIo::new(
                session.clone(),
                metadata.disk_size_bytes,
                config.block_size_bytes,
                Arc::clone(&invalidation),
            )?;
            let cache =
                Cache::new(config, right).map_err(|_| NetworkClientError::InvalidState("cache"))?;
            MediaPath::CachedRw {
                session,
                cache,
                temp_dir,
            }
        };

        Ok(Self {
            remote_disk_id,
            disk_size_bytes: metadata.disk_size_bytes,
            read_only,
            invalidation,
            path: Some(path),
        })
    }

    pub fn with_invalidation_handler(self, handler: Arc<dyn Fn() + Send + Sync>) -> Self {
        self.invalidation.set(handler);
        self
    }

    pub fn session(&self) -> &DiskSession {
        match self.path.as_ref().expect("network media path should exist") {
            MediaPath::BypassRo { session } => session,
            MediaPath::CachedRw { session, .. } => session,
        }
    }
}

impl Media for NetworkMedia {
    fn size_bytes(&self) -> u64 {
        self.disk_size_bytes
    }

    fn read_locked(&self, offset: u64, buffer: &mut [u8]) -> Result<(), BackendError> {
        validate_range(self.disk_size_bytes, offset, buffer.len())?;
        if buffer.is_empty() {
            return Ok(());
        }

        match &self.path {
            Some(MediaPath::BypassRo { session }) => {
                read_session_bytes(session, &self.invalidation, offset, buffer)
            }
            Some(MediaPath::CachedRw { cache, .. }) => cache
                .read_locked(offset, buffer)
                .map_err(map_cache_error_to_backend_error),
            None => Err(BackendError::SessionNotOpen),
        }
    }

    fn write_locked(&self, offset: u64, data: &[u8]) -> Result<(), BackendError> {
        validate_range(self.disk_size_bytes, offset, data.len())?;
        if self.read_only {
            return Err(BackendError::InvalidParameter);
        }
        if data.is_empty() {
            return Ok(());
        }

        match &self.path {
            Some(MediaPath::BypassRo { session }) => {
                write_session_bytes(session, &self.invalidation, offset, data)
            }
            Some(MediaPath::CachedRw { cache, .. }) => cache
                .write_locked(offset, data)
                .map_err(map_cache_error_to_backend_error),
            None => Err(BackendError::SessionNotOpen),
        }
    }
}

impl Drop for NetworkMedia {
    fn drop(&mut self) {
        let Some(path) = self.path.take() else {
            return;
        };

        if let MediaPath::CachedRw {
            session,
            cache,
            temp_dir,
        } = path
        {
            if session.ensure_usable().is_ok() {
                let _ = cache.wait_for_quiesce(CACHE_QUIESCE_TIMEOUT);
            }
            drop(cache);
            let _ = fs::remove_dir_all(temp_dir);
        }
    }
}

impl fmt::Debug for InvalidationDispatch {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        formatter
            .debug_struct("InvalidationDispatch")
            .field(
                "has_handler",
                &self
                    .handler
                    .lock()
                    .expect("invalidation handler poisoned")
                    .is_some(),
            )
            .finish()
    }
}

impl InvalidationDispatch {
    fn set(&self, handler: Arc<dyn Fn() + Send + Sync>) {
        *self.handler.lock().expect("invalidation handler poisoned") = Some(handler);
    }

    fn fire_if_terminal(&self, error: &NetworkClientError) {
        if !is_terminal_media_error(error) {
            return;
        }
        if let Some(handler) = self
            .handler
            .lock()
            .expect("invalidation handler poisoned")
            .as_ref()
            .cloned()
        {
            handler();
        }
    }
}

impl DiskSessionAtIo {
    fn new(
        session: DiskSession,
        disk_size_bytes: u64,
        block_size_bytes: u32,
        invalidation: Arc<InvalidationDispatch>,
    ) -> Result<Self, NetworkClientError> {
        let logical_size_bytes =
            align_up_capacity_bytes(disk_size_bytes, u64::from(block_size_bytes))
                .ok_or(NetworkClientError::InvalidArgument("disk_size_bytes"))?;
        Ok(Self {
            session,
            disk_size_bytes,
            logical_size_bytes,
            block_size_bytes,
            invalidation,
        })
    }

    fn block_size_usize(&self) -> usize {
        self.block_size_bytes as usize
    }

    fn validate_request(&self, offset: u64, length: usize) -> Result<(), CacheError> {
        let block_size = self.block_size_usize();
        if offset % u64::from(self.block_size_bytes) != 0 || length != block_size {
            return Err(CacheError::MisalignedRightIo {
                offset,
                length,
                block_size,
            });
        }

        let end = offset
            .checked_add(
                u64::try_from(length).map_err(|_| CacheError::InvalidRange { offset, length })?,
            )
            .ok_or(CacheError::InvalidRange { offset, length })?;
        if end > self.logical_size_bytes {
            return Err(CacheError::InvalidRange { offset, length });
        }
        Ok(())
    }
}

impl AtIo for DiskSessionAtIo {
    fn read_at(&self, offset: u64, buffer: &mut [u8]) -> Result<(), CacheError> {
        self.validate_request(offset, buffer.len())?;
        let visible_len = visible_disk_backed_length(self.disk_size_bytes, offset, buffer.len());
        let (visible, zero_fill) = buffer.split_at_mut(visible_len);
        if !visible.is_empty() {
            read_session_bytes_for_cache(&self.session, &self.invalidation, offset, visible)?;
        }
        zero_fill.fill(0);
        Ok(())
    }

    fn write_at(&self, offset: u64, data: &[u8]) -> Result<(), CacheError> {
        self.validate_request(offset, data.len())?;
        let visible_len = visible_disk_backed_length(self.disk_size_bytes, offset, data.len());
        if visible_len == 0 {
            return Ok(());
        }
        write_session_bytes_for_cache(
            &self.session,
            &self.invalidation,
            offset,
            &data[..visible_len],
        )
    }
}

fn default_cache_config(temp_dir: PathBuf) -> CacheConfig {
    CacheConfig {
        fifo_capacity_blocks: CACHE_FIFO_CAPACITY_BLOCKS,
        lru_capacity_blocks: CACHE_LRU_CAPACITY_BLOCKS,
        block_size_bytes: CACHE_BLOCK_SIZE_BYTES,
        dirty_scan_interval: CACHE_DIRTY_SCAN_INTERVAL,
        temp_max_files: CACHE_TEMP_MAX_FILES,
        temp_dir,
    }
}

fn prepare_cache_temp_dir(session: &DiskSession, remote_disk_id: &str) -> std::io::Result<PathBuf> {
    let server_addr = sanitize_path_component(session.connection().endpoint().address());
    let remote_disk_id = sanitize_path_component(remote_disk_id);
    let path = std::env::temp_dir()
        .join(CACHE_TEMP_ROOT_NAME)
        .join(server_addr)
        .join(remote_disk_id);
    let _ = fs::remove_dir_all(&path);
    fs::create_dir_all(&path)?;
    Ok(path)
}

fn sanitize_path_component(value: &str) -> String {
    let mut output = String::with_capacity(value.len().max(1));
    for ch in value.chars() {
        let is_invalid =
            ch.is_control() || matches!(ch, '<' | '>' | ':' | '"' | '/' | '\\' | '|' | '?' | '*');
        output.push(if is_invalid { '_' } else { ch });
    }
    if output.is_empty() {
        output.push('_');
    }
    output
}

fn validate_range(disk_size_bytes: u64, offset: u64, length: usize) -> Result<(), BackendError> {
    let end = offset
        .checked_add(u64::try_from(length).map_err(|_| BackendError::InvalidParameter)?)
        .ok_or(BackendError::InvalidParameter)?;
    if end > disk_size_bytes {
        return Err(BackendError::InvalidParameter);
    }
    Ok(())
}

fn align_up_capacity_bytes(size_bytes: u64, alignment_bytes: u64) -> Option<u64> {
    if alignment_bytes == 0 || size_bytes == 0 {
        return Some(size_bytes);
    }

    let remainder = size_bytes % alignment_bytes;
    if remainder == 0 {
        return Some(size_bytes);
    }

    size_bytes.checked_add(alignment_bytes - remainder)
}

fn visible_disk_backed_length(disk_size_bytes: u64, offset: u64, request_length: usize) -> usize {
    if request_length == 0 || offset >= disk_size_bytes {
        return 0;
    }

    (disk_size_bytes - offset).min(request_length as u64) as usize
}

fn read_session_bytes(
    session: &DiskSession,
    invalidation: &InvalidationDispatch,
    offset: u64,
    buffer: &mut [u8],
) -> Result<(), BackendError> {
    let mut remaining = buffer;
    let mut chunk_offset = offset;
    while !remaining.is_empty() {
        let chunk_len = remaining.len().min(MAX_DATA_PLANE_RAW_BYTES as usize);
        let (chunk, rest) = remaining.split_at_mut(chunk_len);
        if let Err(error) = session.read_at(chunk_offset, chunk) {
            invalidation.fire_if_terminal(&error);
            return Err(map_network_error_to_backend_error(error));
        }
        chunk_offset = chunk_offset
            .checked_add(u64::try_from(chunk_len).map_err(|_| BackendError::InvalidParameter)?)
            .ok_or(BackendError::InvalidParameter)?;
        remaining = rest;
    }
    Ok(())
}

fn write_session_bytes(
    session: &DiskSession,
    invalidation: &InvalidationDispatch,
    offset: u64,
    data: &[u8],
) -> Result<(), BackendError> {
    let mut remaining = data;
    let mut chunk_offset = offset;
    while !remaining.is_empty() {
        let chunk_len = remaining.len().min(MAX_DATA_PLANE_RAW_BYTES as usize);
        let (chunk, rest) = remaining.split_at(chunk_len);
        if let Err(error) = session.write_at(chunk_offset, chunk) {
            invalidation.fire_if_terminal(&error);
            return Err(map_network_error_to_backend_error(error));
        }
        chunk_offset = chunk_offset
            .checked_add(u64::try_from(chunk_len).map_err(|_| BackendError::InvalidParameter)?)
            .ok_or(BackendError::InvalidParameter)?;
        remaining = rest;
    }
    Ok(())
}

fn read_session_bytes_for_cache(
    session: &DiskSession,
    invalidation: &InvalidationDispatch,
    offset: u64,
    buffer: &mut [u8],
) -> Result<(), CacheError> {
    let mut remaining = buffer;
    let mut chunk_offset = offset;
    while !remaining.is_empty() {
        let chunk_len = remaining.len().min(MAX_DATA_PLANE_RAW_BYTES as usize);
        let (chunk, rest) = remaining.split_at_mut(chunk_len);
        if let Err(error) = session.read_at(chunk_offset, chunk) {
            invalidation.fire_if_terminal(&error);
            return Err(map_network_error_to_cache_error("read_at", error));
        }
        chunk_offset = chunk_offset
            .checked_add(
                u64::try_from(chunk_len)
                    .map_err(|_| CacheError::ArithmeticOverflow("right read chunk offset"))?,
            )
            .ok_or(CacheError::ArithmeticOverflow("right read chunk offset"))?;
        remaining = rest;
    }
    Ok(())
}

fn write_session_bytes_for_cache(
    session: &DiskSession,
    invalidation: &InvalidationDispatch,
    offset: u64,
    data: &[u8],
) -> Result<(), CacheError> {
    let mut remaining = data;
    let mut chunk_offset = offset;
    while !remaining.is_empty() {
        let chunk_len = remaining.len().min(MAX_DATA_PLANE_RAW_BYTES as usize);
        let (chunk, rest) = remaining.split_at(chunk_len);
        if let Err(error) = session.write_at(chunk_offset, chunk) {
            invalidation.fire_if_terminal(&error);
            return Err(map_network_error_to_cache_error("write_at", error));
        }
        chunk_offset = chunk_offset
            .checked_add(
                u64::try_from(chunk_len)
                    .map_err(|_| CacheError::ArithmeticOverflow("right write chunk offset"))?,
            )
            .ok_or(CacheError::ArithmeticOverflow("right write chunk offset"))?;
        remaining = rest;
    }
    Ok(())
}

fn map_network_error_to_cache_error(
    operation: &'static str,
    error: NetworkClientError,
) -> CacheError {
    let kind = match error {
        NetworkClientError::InvalidArgument(_)
        | NetworkClientError::InvalidState(_)
        | NetworkClientError::OpenRejected
        | NetworkClientError::InvalidIo(_)
        | NetworkClientError::IoFailed => RightIoErrorKind::InvalidInput,
        NetworkClientError::SessionUnavailable
        | NetworkClientError::UnauthorizedDisk { .. }
        | NetworkClientError::AlreadyConnected
        | NetworkClientError::PendingRequestConflict { .. }
        | NetworkClientError::UnknownPendingRequest { .. }
        | NetworkClientError::Protocol(_)
        | NetworkClientError::Transport(_)
        | NetworkClientError::Crypto(_)
        | NetworkClientError::Unimplemented(_) => RightIoErrorKind::Unavailable,
    };

    CacheError::RightIo {
        operation,
        kind,
        detail: error.to_string(),
    }
}

fn map_cache_error_to_backend_error(error: CacheError) -> BackendError {
    match error {
        CacheError::RightIo {
            kind: RightIoErrorKind::Unavailable,
            ..
        }
        | CacheError::Stopped => BackendError::SessionNotOpen,
        CacheError::InvalidConfig(_)
        | CacheError::InvalidRange { .. }
        | CacheError::BufferTooSmall { .. }
        | CacheError::ArithmeticOverflow(_)
        | CacheError::InvalidBlockDataLength { .. }
        | CacheError::InvalidValidLength { .. }
        | CacheError::MisalignedRightIo { .. }
        | CacheError::TempIo { .. }
        | CacheError::RightIo { .. }
        | CacheError::TimedOut { .. }
        | CacheError::ResidentBlockAlreadyExists { .. }
        | CacheError::InvariantViolation(_)
        | CacheError::NotImplemented => BackendError::InvalidParameter,
    }
}

fn map_network_error_to_backend_error(error: NetworkClientError) -> BackendError {
    match error {
        NetworkClientError::InvalidArgument(_)
        | NetworkClientError::InvalidState(_)
        | NetworkClientError::OpenRejected
        | NetworkClientError::InvalidIo(_)
        | NetworkClientError::IoFailed => BackendError::InvalidParameter,
        NetworkClientError::SessionUnavailable
        | NetworkClientError::UnauthorizedDisk { .. }
        | NetworkClientError::AlreadyConnected
        | NetworkClientError::PendingRequestConflict { .. }
        | NetworkClientError::UnknownPendingRequest { .. }
        | NetworkClientError::Protocol(_)
        | NetworkClientError::Transport(_)
        | NetworkClientError::Crypto(_)
        | NetworkClientError::Unimplemented(_) => BackendError::SessionNotOpen,
    }
}

fn is_terminal_media_error(error: &NetworkClientError) -> bool {
    matches!(
        error,
        NetworkClientError::SessionUnavailable
            | NetworkClientError::Transport(_)
            | NetworkClientError::Protocol(_)
    )
}

#[cfg(test)]
mod tests {
    use super::NetworkMedia;
    use backend_rust::BackendError;
    use backend_rust::Media;
    use network_core::client::DiskSession;
    use network_core::client::SessionMetadata;
    use network_core::test_support::clear_session;
    use network_core::test_support::stage_connection;
    use network_core::transport::TransportEndpoint;
    use std::sync::Arc;
    use std::sync::atomic::AtomicUsize;
    use std::sync::atomic::Ordering;

    #[test]
    fn read_locked_marks_media_invalid_when_session_is_terminal() {
        let connection = stage_connection(TransportEndpoint::new("127.0.0.1:1"), 17);
        let session = DiskSession::new(connection.clone(), 17).expect("session should build");
        clear_session(&connection, 17);

        let invalidations = Arc::new(AtomicUsize::new(0));
        let media = NetworkMedia::bind(
            "A1b2C3d4E5f6G7h8",
            session,
            SessionMetadata {
                disk_size_bytes: 4096,
                read_only: false,
                backend_id: [0; 16],
            },
            false,
        )
        .expect("bind should succeed")
        .with_invalidation_handler({
            let invalidations = Arc::clone(&invalidations);
            Arc::new(move || {
                invalidations.fetch_add(1, Ordering::SeqCst);
            })
        });

        let mut buffer = [0u8; 8];
        let error = media
            .read_locked(0, &mut buffer)
            .expect_err("read should fail");
        assert_eq!(error, BackendError::SessionNotOpen);
        assert_eq!(invalidations.load(Ordering::SeqCst), 1);
    }

    #[test]
    fn configured_read_only_rejects_write_when_metadata_is_writable() {
        let connection = stage_connection(TransportEndpoint::new("127.0.0.1:2"), 19);
        let session = DiskSession::new(connection, 19).expect("session should build");

        let media = NetworkMedia::bind(
            "Z9y8X7w6V5u4T3s2",
            session,
            SessionMetadata {
                disk_size_bytes: 4096,
                read_only: false,
                backend_id: [0; 16],
            },
            true,
        )
        .expect("bind should succeed");

        let error = media
            .write_locked(0, &[1, 2, 3, 4])
            .expect_err("write should be rejected");
        assert_eq!(error, BackendError::InvalidParameter);
    }
}
