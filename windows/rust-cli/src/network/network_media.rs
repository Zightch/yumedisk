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

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct NetworkCacheDefaults {
    pub fifo_capacity_blocks: usize,
    pub lru_capacity_blocks: usize,
    pub block_size_bytes: u32,
    pub temp_max_files: usize,
}

impl Default for NetworkCacheDefaults {
    fn default() -> Self {
        Self {
            fifo_capacity_blocks: CACHE_FIFO_CAPACITY_BLOCKS,
            lru_capacity_blocks: CACHE_LRU_CAPACITY_BLOCKS,
            block_size_bytes: CACHE_BLOCK_SIZE_BYTES,
            temp_max_files: CACHE_TEMP_MAX_FILES,
        }
    }
}

pub struct NetworkMedia {
    disk_id: String,
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
            .field("disk_id", &self.disk_id)
            .field("session_id", &self.session().session_id())
            .field("disk_size_bytes", &self.disk_size_bytes)
            .field("read_only", &self.read_only)
            .finish()
    }
}

impl NetworkMedia {
    pub fn bind(
        disk_id: impl Into<String>,
        session: DiskSession,
        metadata: SessionMetadata,
        cache_defaults: NetworkCacheDefaults,
    ) -> Result<Self, NetworkClientError> {
        let disk_id = disk_id.into();
        if disk_id.is_empty() {
            return Err(NetworkClientError::InvalidArgument("disk_id"));
        }
        if metadata.disk_size_bytes == 0 {
            return Err(NetworkClientError::InvalidArgument("disk_size_bytes"));
        }

        let invalidation = Arc::new(InvalidationDispatch::default());
        let read_only = metadata.read_only;
        let path = if read_only {
            MediaPath::BypassRo { session }
        } else {
            let temp_dir = prepare_cache_temp_dir(&session, &disk_id)
                .map_err(|_| NetworkClientError::InvalidState("cache_temp_dir"))?;
            let config = default_cache_config(cache_defaults, temp_dir.clone());
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
            disk_id,
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

    pub fn disk_id(&self) -> &str {
        self.disk_id.as_str()
    }

    pub fn session(&self) -> &DiskSession {
        match self.path.as_ref().expect("network media path should exist") {
            MediaPath::BypassRo { session } => session,
            MediaPath::CachedRw { session, .. } => session,
        }
    }

    pub fn read_only(&self) -> bool {
        self.read_only
    }

    #[cfg(test)]
    fn cache_defaults(&self) -> Option<NetworkCacheDefaults> {
        match self.path.as_ref().expect("network media path should exist") {
            MediaPath::BypassRo { .. } => None,
            MediaPath::CachedRw { cache, .. } => {
                let config = cache.config();
                Some(NetworkCacheDefaults {
                    fifo_capacity_blocks: config.fifo_capacity_blocks,
                    lru_capacity_blocks: config.lru_capacity_blocks,
                    block_size_bytes: config.block_size_bytes,
                    temp_max_files: config.temp_max_files,
                })
            }
        }
    }

    fn read_direct(
        &self,
        session: &DiskSession,
        offset: u64,
        buffer: &mut [u8],
    ) -> Result<(), BackendError> {
        read_session_bytes(session, &self.invalidation, offset, buffer)
    }

    fn write_direct(
        &self,
        session: &DiskSession,
        offset: u64,
        data: &[u8],
    ) -> Result<(), BackendError> {
        write_session_bytes(session, &self.invalidation, offset, data)
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
            Some(MediaPath::BypassRo { session }) => self.read_direct(session, offset, buffer),
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
            Some(MediaPath::BypassRo { session }) => self.write_direct(session, offset, data),
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

fn default_cache_config(cache_defaults: NetworkCacheDefaults, temp_dir: PathBuf) -> CacheConfig {
    CacheConfig {
        fifo_capacity_blocks: cache_defaults.fifo_capacity_blocks,
        lru_capacity_blocks: cache_defaults.lru_capacity_blocks,
        block_size_bytes: cache_defaults.block_size_bytes,
        dirty_scan_interval: CACHE_DIRTY_SCAN_INTERVAL,
        temp_max_files: cache_defaults.temp_max_files,
        temp_dir,
    }
}

fn prepare_cache_temp_dir(session: &DiskSession, disk_id: &str) -> std::io::Result<PathBuf> {
    let server_addr = sanitize_path_component(session.connection().endpoint().address());
    let disk_id = sanitize_path_component(disk_id);
    let path = std::env::temp_dir()
        .join(CACHE_TEMP_ROOT_NAME)
        .join(server_addr)
        .join(disk_id);
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
    use super::DiskSessionAtIo;
    use super::NetworkCacheDefaults;
    use super::NetworkMedia;
    use super::map_network_error_to_backend_error;
    use backend_rust::BackendError;
    use backend_rust::Media;
    use cache::AtIo;
    use network_core::client::DiskSession;
    use network_core::client::NetworkClientError;
    use network_core::client::SessionMetadata;
    use network_core::protocol::ClientOperationCode;
    use network_core::protocol::FLAG_RESPONSE;
    use network_core::protocol::HEADER_SIZE;
    use network_core::protocol::MAX_DATA_PLANE_RAW_BYTES;
    use network_core::protocol::PROTOCOL_VERSION;
    use network_core::protocol::ProtocolHeader;
    use network_core::protocol::ProtocolStatusCode;
    use network_core::protocol::parse_request_header;
    use network_core::test_support::clear_session;
    use network_core::test_support::expect_client_hello;
    use network_core::test_support::stage_connection;
    use network_core::transport::MAX_FRAME_PAYLOAD_BYTES;
    use network_core::transport::TransportEndpoint;
    use network_core::transport::read_frame_into;
    use network_core::transport::write_frame;
    use std::net::TcpListener;
    use std::sync::Arc;
    use std::sync::mpsc;
    use std::thread;
    use std::time::Duration;

    fn sample_metadata(disk_size_bytes: u64, read_only: bool) -> SessionMetadata {
        SessionMetadata {
            disk_size_bytes,
            read_only,
            backend_id: [0u8; 16],
        }
    }

    fn incompressible_test_data(length: usize) -> Vec<u8> {
        let mut state = 0x9E3779B97F4A7C15u64;
        let mut output = Vec::with_capacity(length);
        for _ in 0..length {
            state ^= state << 7;
            state ^= state >> 9;
            state = state.wrapping_mul(0xA24BAED4963EE407);
            output.push((state & 0xFF) as u8);
        }
        output
    }

    fn default_cache_defaults() -> NetworkCacheDefaults {
        NetworkCacheDefaults::default()
    }

    #[test]
    fn bind_requires_explicit_disk_id_and_metadata() {
        let connection = stage_connection(TransportEndpoint::new("127.0.0.1:9000"), 7);
        let session = DiskSession::new(connection, 7).expect("session should build");

        let error = NetworkMedia::bind(
            "",
            session,
            sample_metadata(2048, false),
            default_cache_defaults(),
        )
        .expect_err("bind should fail");
        assert_eq!(error.to_string(), "invalid-argument: disk_id");
    }

    #[test]
    fn read_locked_splits_large_requests_by_raw_limit_on_bypass_path() {
        let split_len = MAX_DATA_PLANE_RAW_BYTES as usize;
        let listener = TcpListener::bind("127.0.0.1:0").expect("bind should succeed");
        let address = listener.local_addr().expect("local addr should succeed");

        let server = thread::spawn(move || {
            let (mut stream, _) = listener.accept().expect("accept should succeed");
            expect_client_hello(&mut stream);
            let mut buffer = vec![0u8; MAX_FRAME_PAYLOAD_BYTES];

            let first = read_frame_into(&mut stream, &mut buffer)
                .expect("read first request should succeed")
                .to_vec();
            let first_header = parse_request_header(&first).expect("parse first");
            assert_eq!(first_header.op_code, ClientOperationCode::ReadAt);
            assert_eq!(&first[HEADER_SIZE..HEADER_SIZE + 8], &0u64.to_be_bytes());
            assert_eq!(
                &first[HEADER_SIZE + 8..HEADER_SIZE + 12],
                &MAX_DATA_PLANE_RAW_BYTES.to_be_bytes()
            );
            let mut first_body = Vec::with_capacity(split_len + 1);
            first_body.push(0);
            first_body.extend(vec![b'A'; split_len]);
            let first_response = ProtocolHeader {
                protocol_version: PROTOCOL_VERSION,
                header_len: HEADER_SIZE as u8,
                op_code: ClientOperationCode::ReadAt,
                flags: FLAG_RESPONSE,
                status_code: ProtocolStatusCode::Ok,
                reserved: 0,
                request_id: first_header.request_id,
                session_id: first_header.session_id,
            }
            .encode(&first_body);
            write_frame(&mut stream, &first_response).expect("write first response");

            let second = read_frame_into(&mut stream, &mut buffer)
                .expect("read second request should succeed")
                .to_vec();
            let second_header = parse_request_header(&second).expect("parse second");
            assert_eq!(second_header.op_code, ClientOperationCode::ReadAt);
            assert_eq!(
                &second[HEADER_SIZE..HEADER_SIZE + 8],
                &u64::from(MAX_DATA_PLANE_RAW_BYTES).to_be_bytes()
            );
            assert_eq!(
                &second[HEADER_SIZE + 8..HEADER_SIZE + 12],
                &4u32.to_be_bytes()
            );
            let second_body = [0, b'B', b'B', b'B', b'B'];
            let second_response = ProtocolHeader {
                protocol_version: PROTOCOL_VERSION,
                header_len: HEADER_SIZE as u8,
                op_code: ClientOperationCode::ReadAt,
                flags: FLAG_RESPONSE,
                status_code: ProtocolStatusCode::Ok,
                reserved: 0,
                request_id: second_header.request_id,
                session_id: second_header.session_id,
            }
            .encode(&second_body);
            write_frame(&mut stream, &second_response).expect("write second response");
        });

        let connection = stage_connection(TransportEndpoint::new(address.to_string()), 77);
        connection.connect().expect("connect should succeed");
        let session = DiskSession::new(connection.clone(), 77).expect("session should build");
        let media = NetworkMedia::bind(
            "A1b2C3d4E5f6G7h8",
            session,
            sample_metadata(u64::from(MAX_DATA_PLANE_RAW_BYTES) + 4, true),
            default_cache_defaults(),
        )
        .expect("bind should succeed");
        assert_eq!(media.disk_id(), "A1b2C3d4E5f6G7h8");

        let mut buffer = vec![0u8; split_len + 4];
        media
            .read_locked(0, &mut buffer)
            .expect("read should succeed");
        assert_eq!(&buffer[..split_len], vec![b'A'; split_len]);
        assert_eq!(&buffer[split_len..], b"BBBB");

        connection.close().expect("close should succeed");
        server.join().expect("server should join");
    }

    #[test]
    fn write_locked_flushes_cached_block_before_drop() {
        let listener = TcpListener::bind("127.0.0.1:0").expect("bind should succeed");
        let address = listener.local_addr().expect("local addr should succeed");
        let block_size = 32 * 1024usize;
        let expected_first_block = incompressible_test_data(block_size);
        let expected_first_block_for_server = expected_first_block.clone();

        let server = thread::spawn(move || {
            let (mut stream, _) = listener.accept().expect("accept should succeed");
            expect_client_hello(&mut stream);
            let mut buffer = vec![0u8; MAX_FRAME_PAYLOAD_BYTES];

            let read_request = read_frame_into(&mut stream, &mut buffer)
                .expect("read request should succeed")
                .to_vec();
            let read_header = parse_request_header(&read_request).expect("parse read request");
            assert_eq!(read_header.op_code, ClientOperationCode::ReadAt);
            assert_eq!(
                &read_request[HEADER_SIZE..HEADER_SIZE + 8],
                &0u64.to_be_bytes()
            );
            assert_eq!(
                &read_request[HEADER_SIZE + 8..HEADER_SIZE + 12],
                &(block_size as u32).to_be_bytes()
            );
            let mut read_body = Vec::with_capacity(block_size + 1);
            read_body.push(0);
            read_body.extend_from_slice(&expected_first_block_for_server);
            let read_response = ProtocolHeader {
                protocol_version: PROTOCOL_VERSION,
                header_len: HEADER_SIZE as u8,
                op_code: ClientOperationCode::ReadAt,
                flags: FLAG_RESPONSE,
                status_code: ProtocolStatusCode::Ok,
                reserved: 0,
                request_id: read_header.request_id,
                session_id: read_header.session_id,
            }
            .encode(&read_body);
            write_frame(&mut stream, &read_response).expect("write read response");

            let write_request = read_frame_into(&mut stream, &mut buffer)
                .expect("write request should succeed")
                .to_vec();
            let write_header = parse_request_header(&write_request).expect("parse write request");
            assert_eq!(write_header.op_code, ClientOperationCode::WriteAt);
            assert_eq!(
                &write_request[HEADER_SIZE..HEADER_SIZE + 8],
                &0u64.to_be_bytes()
            );
            assert_eq!(
                &write_request[HEADER_SIZE + 8..HEADER_SIZE + 12],
                &(block_size as u32).to_be_bytes()
            );
            assert_eq!(write_request[HEADER_SIZE + 12], 0);
            let payload = &write_request[HEADER_SIZE + 13..];
            assert_eq!(payload.len(), block_size);
            assert_eq!(&payload[..4], &expected_first_block_for_server[..4]);
            assert_eq!(&payload[4..8], b"WXYZ");
            let write_response = ProtocolHeader {
                protocol_version: PROTOCOL_VERSION,
                header_len: HEADER_SIZE as u8,
                op_code: ClientOperationCode::WriteAt,
                flags: FLAG_RESPONSE,
                status_code: ProtocolStatusCode::Ok,
                reserved: 0,
                request_id: write_header.request_id,
                session_id: write_header.session_id,
            }
            .encode(&[]);
            write_frame(&mut stream, &write_response).expect("write write response");
        });

        let connection = stage_connection(TransportEndpoint::new(address.to_string()), 77);
        connection.connect().expect("connect should succeed");
        let session = DiskSession::new(connection.clone(), 77).expect("session should build");
        let media = NetworkMedia::bind(
            "A1b2C3d4E5f6G7h8",
            session,
            sample_metadata(block_size as u64, false),
            default_cache_defaults(),
        )
        .expect("bind should succeed");

        media
            .write_locked(4, b"WXYZ")
            .expect("write should be accepted into cache");
        drop(media);

        connection.close().expect("close should succeed");
        server.join().expect("server should join");
    }

    #[test]
    fn disk_session_at_io_zero_fills_short_tail_reads() {
        let listener = TcpListener::bind("127.0.0.1:0").expect("bind should succeed");
        let address = listener.local_addr().expect("local addr should succeed");

        let server = thread::spawn(move || {
            let (mut stream, _) = listener.accept().expect("accept should succeed");
            expect_client_hello(&mut stream);
            let mut buffer = vec![0u8; MAX_FRAME_PAYLOAD_BYTES];

            let read_request = read_frame_into(&mut stream, &mut buffer)
                .expect("read request should succeed")
                .to_vec();
            let header = parse_request_header(&read_request).expect("parse request");
            assert_eq!(header.op_code, ClientOperationCode::ReadAt);
            assert_eq!(
                &read_request[HEADER_SIZE..HEADER_SIZE + 8],
                &96u64.to_be_bytes()
            );
            assert_eq!(
                &read_request[HEADER_SIZE + 8..HEADER_SIZE + 12],
                &4u32.to_be_bytes()
            );

            let response = ProtocolHeader {
                protocol_version: PROTOCOL_VERSION,
                header_len: HEADER_SIZE as u8,
                op_code: ClientOperationCode::ReadAt,
                flags: FLAG_RESPONSE,
                status_code: ProtocolStatusCode::Ok,
                reserved: 0,
                request_id: header.request_id,
                session_id: header.session_id,
            }
            .encode(&[0, 9, 8, 7, 6]);
            write_frame(&mut stream, &response).expect("write response");
        });

        let connection = stage_connection(TransportEndpoint::new(address.to_string()), 77);
        connection.connect().expect("connect should succeed");
        let session = DiskSession::new(connection.clone(), 77).expect("session should build");
        let right = DiskSessionAtIo::new(
            session,
            100,
            32,
            Arc::new(super::InvalidationDispatch::default()),
        )
        .expect("adapter should build");

        let mut buffer = [0xFFu8; 32];
        right
            .read_at(96, &mut buffer)
            .expect("tail read should succeed");
        assert_eq!(&buffer[..4], &[9, 8, 7, 6]);
        assert!(buffer[4..].iter().all(|byte| *byte == 0));

        connection.close().expect("close should succeed");
        server.join().expect("server should join");
    }

    #[test]
    fn disk_session_at_io_drops_bytes_past_actual_tail_on_write() {
        let listener = TcpListener::bind("127.0.0.1:0").expect("bind should succeed");
        let address = listener.local_addr().expect("local addr should succeed");

        let server = thread::spawn(move || {
            let (mut stream, _) = listener.accept().expect("accept should succeed");
            expect_client_hello(&mut stream);
            let mut buffer = vec![0u8; MAX_FRAME_PAYLOAD_BYTES];

            let write_request = read_frame_into(&mut stream, &mut buffer)
                .expect("write request should succeed")
                .to_vec();
            let header = parse_request_header(&write_request).expect("parse write request");
            assert_eq!(header.op_code, ClientOperationCode::WriteAt);
            assert_eq!(
                &write_request[HEADER_SIZE..HEADER_SIZE + 8],
                &96u64.to_be_bytes()
            );
            assert_eq!(
                &write_request[HEADER_SIZE + 8..HEADER_SIZE + 12],
                &4u32.to_be_bytes()
            );
            assert_eq!(write_request[HEADER_SIZE + 12], 0);
            assert_eq!(&write_request[HEADER_SIZE + 13..], &[1, 2, 3, 4]);

            let response = ProtocolHeader {
                protocol_version: PROTOCOL_VERSION,
                header_len: HEADER_SIZE as u8,
                op_code: ClientOperationCode::WriteAt,
                flags: FLAG_RESPONSE,
                status_code: ProtocolStatusCode::Ok,
                reserved: 0,
                request_id: header.request_id,
                session_id: header.session_id,
            }
            .encode(&[]);
            write_frame(&mut stream, &response).expect("write response");
        });

        let connection = stage_connection(TransportEndpoint::new(address.to_string()), 77);
        connection.connect().expect("connect should succeed");
        let session = DiskSession::new(connection.clone(), 77).expect("session should build");
        let right = DiskSessionAtIo::new(
            session,
            100,
            32,
            Arc::new(super::InvalidationDispatch::default()),
        )
        .expect("adapter should build");

        let data = [
            1u8, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23,
            24, 25, 26, 27, 28, 29, 30, 31, 32,
        ];
        right
            .write_at(96, &data)
            .expect("tail write should succeed");

        connection.close().expect("close should succeed");
        server.join().expect("server should join");
    }

    #[test]
    fn network_media_maps_network_failures_to_backend_errors() {
        assert_eq!(
            map_network_error_to_backend_error(NetworkClientError::SessionUnavailable),
            BackendError::SessionNotOpen
        );
        assert_eq!(
            map_network_error_to_backend_error(NetworkClientError::Transport(
                "disconnect".to_string()
            )),
            BackendError::SessionNotOpen
        );
        assert_eq!(
            map_network_error_to_backend_error(NetworkClientError::IoFailed),
            BackendError::InvalidParameter
        );
        assert_eq!(
            map_network_error_to_backend_error(NetworkClientError::InvalidIo("read_only")),
            BackendError::InvalidParameter
        );
    }

    #[test]
    fn network_media_keeps_cache_defaults_per_instance() {
        let defaults_a = NetworkCacheDefaults {
            fifo_capacity_blocks: 16,
            lru_capacity_blocks: 32,
            block_size_bytes: 48 * 1024,
            temp_max_files: 8,
        };
        let defaults_b = NetworkCacheDefaults {
            fifo_capacity_blocks: 4,
            lru_capacity_blocks: 12,
            block_size_bytes: 96 * 1024,
            temp_max_files: 3,
        };

        let session_a = DiskSession::new(
            stage_connection(TransportEndpoint::new("127.0.0.1:9000"), 7),
            7,
        )
        .expect("session should build");
        let media_a = NetworkMedia::bind(
            "A1b2C3d4E5f6G7h8",
            session_a,
            sample_metadata(4096, false),
            defaults_a,
        )
        .expect("bind should succeed");

        let session_b = DiskSession::new(
            stage_connection(TransportEndpoint::new("127.0.0.1:9001"), 8),
            8,
        )
        .expect("session should build");
        let media_b = NetworkMedia::bind(
            "H8g7F6e5D4c3B2a1",
            session_b,
            sample_metadata(4096, false),
            defaults_b,
        )
        .expect("bind should succeed");

        assert_eq!(media_a.cache_defaults(), Some(defaults_a));
        assert_eq!(media_b.cache_defaults(), Some(defaults_b));
    }

    #[test]
    fn network_media_calls_invalidation_handler_on_cached_path_session_loss() {
        let listener = TcpListener::bind("127.0.0.1:0").expect("bind should succeed");
        let address = listener.local_addr().expect("local addr should succeed");

        let server = thread::spawn(move || {
            let (mut stream, _) = listener.accept().expect("accept should succeed");
            expect_client_hello(&mut stream);
            let mut buffer = vec![0u8; MAX_FRAME_PAYLOAD_BYTES];
            let request = read_frame_into(&mut stream, &mut buffer)
                .expect("read request should succeed")
                .to_vec();
            let header = parse_request_header(&request).expect("parse request");
            assert_eq!(header.op_code, ClientOperationCode::ReadAt);
            let response = ProtocolHeader {
                protocol_version: PROTOCOL_VERSION,
                header_len: HEADER_SIZE as u8,
                op_code: ClientOperationCode::ReadAt,
                flags: FLAG_RESPONSE,
                status_code: ProtocolStatusCode::ErrSessionUnavailable,
                reserved: 0,
                request_id: header.request_id,
                session_id: header.session_id,
            }
            .encode(&[]);
            write_frame(&mut stream, &response).expect("write response");
            thread::sleep(Duration::from_millis(20));
        });

        let connection = stage_connection(TransportEndpoint::new(address.to_string()), 77);
        connection.connect().expect("connect should succeed");
        let session = DiskSession::new(connection.clone(), 77).expect("session should build");
        let (invalidate_tx, invalidate_rx) = mpsc::channel();
        let media = NetworkMedia::bind(
            "A1b2C3d4E5f6G7h8",
            session,
            sample_metadata(4096, false),
            default_cache_defaults(),
        )
        .expect("bind should succeed")
        .with_invalidation_handler(Arc::new(move || {
            let _ = invalidate_tx.send(());
        }));

        let mut buffer = [0u8; 4];
        let error = media
            .read_locked(0, &mut buffer)
            .expect_err("read should fail");
        assert_eq!(error, BackendError::SessionNotOpen);
        invalidate_rx
            .recv_timeout(Duration::from_millis(200))
            .expect("invalidation handler should fire");

        connection.close().expect("close should succeed");
        server.join().expect("server should join");
    }

    #[test]
    fn network_media_calls_invalidation_handler_on_session_loss() {
        let connection = stage_connection(TransportEndpoint::new("127.0.0.1:1"), 17);
        let session = DiskSession::new(connection.clone(), 17).expect("session should build");
        clear_session(&connection, 17);

        let invalidations = Arc::new(std::sync::atomic::AtomicUsize::new(0));
        let media = NetworkMedia::bind(
            "A1b2C3d4E5f6G7h8",
            session,
            sample_metadata(4096, true),
            default_cache_defaults(),
        )
        .expect("bind should succeed")
        .with_invalidation_handler({
            let invalidations = Arc::clone(&invalidations);
            Arc::new(move || {
                invalidations.fetch_add(1, std::sync::atomic::Ordering::SeqCst);
            })
        });

        let mut buffer = [0u8; 8];
        let error = media
            .read_locked(0, &mut buffer)
            .expect_err("read should fail");
        assert_eq!(error, BackendError::SessionNotOpen);
        assert_eq!(invalidations.load(std::sync::atomic::Ordering::SeqCst), 1);
    }
}
