use std::collections::BTreeMap;
use std::collections::BTreeSet;
use std::sync::Arc;
use std::sync::Mutex;

use backend_rust::BackendContext;
use backend_rust::BackendStatsSnapshot;
use backend_rust::DebugSnapshot;
use backend_rust::DiskConfig;
use backend_rust::ManagedDiskEvent;
use backend_rust::ManagedDiskResponse;
use backend_rust::ManagedDiskResponseType;
use backend_rust::ManagedDiskSnapshot;
use backend_rust::ManagedSessionNotice;
use backend_rust::ManagedSessionNoticeType;
use backend_rust::Media;
use backend_rust::YUMEDISK_MAX_TARGETS;
use backend_rust::YUMEDISK_MAX_USABLE_TARGET_ID;
use network_core::client::ConnectionAuthenticator;
use network_core::client::DiskSession;
use network_core::client::GatewayConnection;
use network_core::client::NetworkClientError;
use network_core::client::SessionCloseNotice;
use network_core::client::SessionDataChangedNotice;
use network_core::client::SessionDescriber;
use network_core::client::SessionOpener;
use network_core::transport::TransportEndpoint;

use crate::NetworkMedia;
use crate::cli::local::LocalBindingKind;
use crate::cli::local::SharedMemoryRegistry;
use crate::cli::local::SharedMemorySnapshot;

pub type AppResult<T> = Result<T, String>;

#[derive(Debug, Clone, Copy)]
pub struct CreateLocalDiskRequest {
    pub source: CreateLocalDiskSource,
    pub read_only: bool,
    pub target_id: Option<u32>,
}

#[derive(Debug, Clone, Copy)]
pub enum CreateLocalDiskSource {
    SizeMib(u64),
    SharedMemoryId(u64),
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum RemoveTargetSelector {
    All,
    One(u32),
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum RemoveSharedMemorySelector {
    All,
    One(u64),
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct CreateSharedMemoryResult {
    pub smid: u64,
    pub size_bytes: u64,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct NetworkMountResult {
    pub target_id: u32,
    pub addr: String,
    pub disk_id: String,
    pub session_id: u64,
    pub disk_size_bytes: u64,
    pub read_only: bool,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct HostManagedDiskEvent {
    pub event: ManagedDiskEvent,
    pub preserved_smid: Option<u64>,
}

#[derive(Debug, Clone)]
struct MountedNetworkDisk {
    addr: String,
    disk_id: String,
    remote_backend_id: [u8; 16],
    session: DiskSession,
    disk_size_bytes: u64,
    read_only: bool,
}

#[derive(Debug, Default, Clone)]
struct NetworkMountRegistry {
    state: Arc<Mutex<NetworkMountState>>,
}

#[derive(Debug, Default)]
struct NetworkMountState {
    connections: BTreeMap<String, Arc<GatewayConnection>>,
    mounted: BTreeMap<u32, MountedNetworkDisk>,
    pending_cleanup_targets: BTreeSet<u32>,
    pending_data_changed_targets: BTreeSet<u32>,
}

pub struct CliHost {
    context: BackendContext,
    shared_memory: SharedMemoryRegistry,
    network_mounts: NetworkMountRegistry,
}

impl CliHost {
    pub fn open() -> AppResult<Self> {
        let context = BackendContext::new();
        if !context.open() {
            let error = context
                .snapshot_log_lines()
                .last()
                .cloned()
                .unwrap_or_else(|| "open-failed".to_string());
            return Err(error);
        }

        Ok(Self {
            context,
            shared_memory: SharedMemoryRegistry::default(),
            network_mounts: NetworkMountRegistry::default(),
        })
    }

    pub fn shutdown(&mut self) {
        self.remove_all_disks_for_shutdown();
        self.context.close();
        self.network_mounts.clear();
    }

    pub fn query_session_state_text(&self) -> String {
        self.context.query_session_state_text()
    }

    pub fn snapshot_managed_disks(&self) -> Vec<ManagedDiskSnapshot> {
        self.context.snapshot_managed_disks()
    }

    pub fn query_backend_stats(&self) -> AppResult<BackendStatsSnapshot> {
        let mut stats = BackendStatsSnapshot::default();
        let mut error_text = String::new();
        if !self
            .context
            .query_backend_stats(&mut stats, Some(&mut error_text))
        {
            if error_text.is_empty() {
                error_text = "stats-query-failed".to_string();
            }
            return Err(error_text);
        }
        Ok(stats)
    }

    pub fn query_debug_snapshot(&self) -> AppResult<DebugSnapshot> {
        let mut snapshot = DebugSnapshot::default();
        let mut error_text = String::new();
        if !self
            .context
            .query_debug_snapshot(&mut snapshot, Some(&mut error_text))
        {
            if error_text.is_empty() {
                error_text = "debug-query-failed".to_string();
            }
            return Err(error_text);
        }
        Ok(snapshot)
    }

    pub fn create_shared_memory(&mut self, size_mib: u64) -> AppResult<CreateSharedMemoryResult> {
        let size_bytes = mib_to_bytes(size_mib)?;
        let smid = self.shared_memory.create_shared_memory(size_bytes)?;
        Ok(CreateSharedMemoryResult { smid, size_bytes })
    }

    pub fn create_local_disk(&mut self, request: CreateLocalDiskRequest) -> AppResult<u32> {
        let target_id = request
            .target_id
            .unwrap_or_else(|| self.context.find_first_free_target());
        if target_id >= YUMEDISK_MAX_TARGETS {
            return Err("no-free-target".to_string());
        }

        let (dense_mem, disk_size_bytes, binding) = match request.source {
            CreateLocalDiskSource::SizeMib(size_mib) => {
                let size_bytes = mib_to_bytes(size_mib)?;
                let (media, prepared_size, binding) =
                    self.shared_memory.prepare_dedicated_media(size_bytes)?;
                (media, prepared_size, binding)
            }
            CreateLocalDiskSource::SharedMemoryId(smid) => {
                let (media, size_bytes) = self.shared_memory.prepare_shared_media(smid)?;
                (media, size_bytes, LocalBindingKind::Shared { smid })
            }
        };
        let disk_config = DiskConfig {
            target_id,
            disk_size_bytes,
            read_only: request.read_only,
            ..DiskConfig::default()
        };

        let mut error_text = String::new();
        if !self.context.create_managed_disk(
            disk_config,
            Box::new(dense_mem),
            Some(&mut error_text),
        ) {
            if error_text.is_empty() {
                error_text = "create-failed".to_string();
            }
            return Err(error_text);
        }

        if let Err(error) = self
            .shared_memory
            .register_target_binding(target_id, binding)
        {
            let mut remove_error = String::new();
            let _ = self
                .context
                .remove_managed_disk_with_media(target_id, Some(&mut remove_error));
            return Err(error);
        }
        Ok(target_id)
    }

    pub fn mount_network_disk(
        &mut self,
        addr: &str,
        claim_code: &str,
        target_id: Option<u32>,
    ) -> AppResult<NetworkMountResult> {
        let target_id = target_id.unwrap_or_else(|| self.context.find_first_free_target());
        if target_id > YUMEDISK_MAX_USABLE_TARGET_ID {
            return Err("no-free-target".to_string());
        }

        let connection = self
            .network_mounts
            .acquire_connection(addr)
            .map_err(|error| error.to_string())?;
        let authenticator = ConnectionAuthenticator::new(Arc::clone(&connection));
        let auth = authenticator
            .authenticate(claim_code)
            .map_err(|error| error.to_string())?;

        let opener = SessionOpener::new(Arc::clone(&connection));
        let session_id = opener.open(&auth).map_err(|error| error.to_string())?;
        let session = DiskSession::new(Arc::clone(&connection), session_id).map_err(|error| {
            let _ = connection.close();
            error.to_string()
        })?;
        let describer = SessionDescriber::new(Arc::clone(&connection));
        let metadata = describer.describe(session_id).map_err(|error| {
            if close_session_for_cleanup(&session) {
                self.network_mounts
                    .release_connection_after_session_close(&connection);
            }
            error.to_string()
        })?;
        self.network_mounts
            .ensure_unique_mount(addr, auth.disk_id(), metadata.backend_id)
            .map_err(|error| {
                if close_session_for_cleanup(&session) {
                    self.network_mounts
                        .release_connection_after_session_close(&connection);
                }
                error
            })?;

        let media = NetworkMedia::bind(auth.disk_id().to_string(), session.clone(), metadata)
            .map_err(|error| {
                if close_session_for_cleanup(&session) {
                    self.network_mounts
                        .release_connection_after_session_close(&connection);
                }
                error.to_string()
            })?
            .with_invalidation_handler(self.network_mounts.cleanup_marker(target_id));

        let mounted = MountedNetworkDisk::new(
            addr.to_string(),
            auth.disk_id().to_string(),
            metadata.backend_id,
            session.clone(),
            metadata.disk_size_bytes,
            metadata.read_only,
        );

        let disk_config = DiskConfig {
            target_id,
            disk_size_bytes: metadata.disk_size_bytes,
            read_only: metadata.read_only,
            ..DiskConfig::default()
        };

        let mut error_text = String::new();
        let boxed_media: Box<dyn Media> = Box::new(media);
        if !self
            .context
            .create_managed_disk(disk_config, boxed_media, Some(&mut error_text))
        {
            self.network_mounts.clear_cleanup_mark(target_id);
            if close_session_for_cleanup(&session) {
                self.network_mounts
                    .release_connection_after_session_close(&connection);
            }
            if error_text.is_empty() {
                error_text = "create-failed".to_string();
            }
            return Err(error_text);
        }

        self.network_mounts.insert(target_id, mounted.clone());

        Ok(mounted.mount_result(target_id))
    }

    pub fn remove_disk(&mut self, target_id: u32) -> AppResult<()> {
        let mut error_text = String::new();
        let media = self
            .context
            .remove_managed_disk_with_media(target_id, Some(&mut error_text));
        let Some(media) = media else {
            if error_text.is_empty() {
                error_text = "remove-failed".to_string();
            }
            return Err(error_text);
        };
        drop(media);

        if let Some(mounted) = self.network_mounts.take(target_id) {
            let session_closed = close_session_for_cleanup(&mounted.session);
            if session_closed {
                self.network_mounts
                    .release_connection_after_session_close(mounted.session.connection());
            }
        }
        self.network_mounts.clear_cleanup_mark(target_id);
        self.shared_memory.unbind_target(target_id, false);

        Ok(())
    }

    pub fn remove_targets(&mut self, selector: RemoveTargetSelector) -> AppResult<Vec<u32>> {
        let target_ids = match selector {
            RemoveTargetSelector::All => self
                .context
                .snapshot_managed_disks()
                .into_iter()
                .map(|disk| disk.target_id)
                .collect::<Vec<_>>(),
            RemoveTargetSelector::One(target_id) => vec![target_id],
        };

        for target_id in &target_ids {
            self.remove_disk(*target_id)?;
        }
        Ok(target_ids)
    }

    pub fn remove_shared_memory(
        &mut self,
        selector: RemoveSharedMemorySelector,
    ) -> AppResult<Vec<u64>> {
        match selector {
            RemoveSharedMemorySelector::All => self.shared_memory.remove_all_shared_memory(),
            RemoveSharedMemorySelector::One(smid) => {
                self.shared_memory.remove_shared_memory(smid)?;
                Ok(vec![smid])
            }
        }
    }

    fn remove_all_disks_for_shutdown(&mut self) {
        let target_ids = self
            .context
            .snapshot_managed_disks()
            .into_iter()
            .map(|disk| disk.target_id)
            .collect::<Vec<_>>();

        for target_id in target_ids {
            let mut error_text = String::new();
            let media = self
                .context
                .remove_managed_disk_with_media(target_id, Some(&mut error_text));
            drop(media);
            if let Some(mounted) = self.network_mounts.take(target_id) {
                mounted.shutdown();
            }
            self.network_mounts.clear_cleanup_mark(target_id);
            self.shared_memory.unbind_target(target_id, false);
        }
    }

    pub fn reap_dead_network_disks(&mut self) -> AppResult<Vec<u32>> {
        let removals = self.network_mounts.cleanup_target_ids();

        let mut removed = Vec::new();
        for target_id in &removals {
            self.remove_disk(*target_id)?;
            removed.push(*target_id);
        }
        for target_id in removals {
            self.network_mounts.clear_cleanup_mark(target_id);
        }
        Ok(removed)
    }

    pub fn network_mounts(&self) -> Vec<NetworkMountResult> {
        self.network_mounts.network_mounts()
    }

    pub fn apply_network_data_changed(&mut self) -> AppResult<Vec<u32>> {
        let target_ids = self.network_mounts.take_data_changed_target_ids();
        let mut changed = Vec::new();
        for target_id in target_ids {
            let mut error_text = String::new();
            if !self
                .context
                .notify_managed_disk_data_changed(target_id, Some(&mut error_text))
            {
                if error_text.is_empty() {
                    error_text = "notify-data-changed-failed".to_string();
                }
                return Err(format!(
                    "notify managed disk data changed target={} failed: {}",
                    target_id, error_text
                ));
            }
            changed.push(target_id);
        }
        Ok(changed)
    }

    pub fn poll_managed_disk_response(&mut self) -> AppResult<Option<ManagedDiskResponse>> {
        let response = self.context.poll_managed_disk_response();
        if let Some(response) = response {
            self.handle_host_response(&response)?;
            return Ok(Some(response));
        }
        Ok(None)
    }

    pub fn poll_managed_disk_event(&mut self) -> AppResult<Option<HostManagedDiskEvent>> {
        let event = self.context.poll_managed_disk_event();
        if let Some(event) = event {
            let notice = self.handle_host_disk_event(&event)?;
            return Ok(Some(notice));
        }
        Ok(None)
    }

    pub fn poll_managed_session_notice(&mut self) -> AppResult<Option<ManagedSessionNotice>> {
        let notice = self.context.poll_managed_session_notice();
        if let Some(notice) = notice {
            self.handle_host_session_notice(&notice)?;
            return Ok(Some(notice));
        }
        Ok(None)
    }

    pub fn snapshot_shared_memory(&self) -> Vec<SharedMemorySnapshot> {
        self.shared_memory.snapshots()
    }

    pub fn debug_read_target_bytes(
        &self,
        target_id: u32,
        offset: u64,
        length: usize,
    ) -> AppResult<Vec<u8>> {
        self.shared_memory
            .read_bound_target_bytes(target_id, offset, length)
    }

    pub fn debug_write_target_bytes(
        &self,
        target_id: u32,
        offset: u64,
        data: &[u8],
    ) -> AppResult<()> {
        self.shared_memory
            .write_bound_target_bytes(target_id, offset, data)
    }

    fn handle_host_response(&mut self, response: &ManagedDiskResponse) -> AppResult<()> {
        match response.response_type {
            ManagedDiskResponseType::DiskRemoved => {
                self.shared_memory.unbind_target(response.target_id, false);
                return Ok(());
            }
            ManagedDiskResponseType::WriteFinalCommitted => {}
            _ => return Ok(()),
        }
        let Some((_smid, siblings)) = self.shared_memory.sibling_targets(response.target_id) else {
            return Ok(());
        };
        let mut first_error = None;
        for target_id in siblings {
            let mut error_text = String::new();
            if !self
                .context
                .notify_managed_disk_data_changed(target_id, Some(&mut error_text))
            {
                if error_text.is_empty() {
                    error_text = "notify-data-changed-failed".to_string();
                }
                if first_error.is_none() {
                    first_error = Some(format!(
                        "notify sibling target={} from target={} failed: {}",
                        target_id, response.target_id, error_text
                    ));
                }
            }
        }
        match first_error {
            Some(error) => Err(error),
            None => Ok(()),
        }
    }

    fn handle_host_disk_event(
        &mut self,
        event: &ManagedDiskEvent,
    ) -> AppResult<HostManagedDiskEvent> {
        Ok(HostManagedDiskEvent {
            event: *event,
            preserved_smid: None,
        })
    }

    fn handle_host_session_notice(&mut self, notice: &ManagedSessionNotice) -> AppResult<()> {
        match notice.notice_type {
            ManagedSessionNoticeType::Broken => Ok(()),
        }
    }
}

fn mib_to_bytes(size_mib: u64) -> AppResult<u64> {
    if size_mib == 0 {
        return Err("disk size mib must be > 0".to_string());
    }
    size_mib
        .checked_mul(1024 * 1024)
        .ok_or_else(|| "disk-size-overflow".to_string())
}

impl MountedNetworkDisk {
    fn new(
        addr: String,
        disk_id: String,
        remote_backend_id: [u8; 16],
        session: DiskSession,
        disk_size_bytes: u64,
        read_only: bool,
    ) -> Self {
        Self {
            addr,
            disk_id,
            remote_backend_id,
            session,
            disk_size_bytes,
            read_only,
        }
    }

    fn shutdown(&self) {
        let _ = self.session.connection().close();
    }

    fn mount_result(&self, target_id: u32) -> NetworkMountResult {
        NetworkMountResult {
            target_id,
            addr: self.addr.clone(),
            disk_id: self.disk_id.clone(),
            session_id: self.session.session_id(),
            disk_size_bytes: self.disk_size_bytes,
            read_only: self.read_only,
        }
    }
}

impl NetworkMountRegistry {
    fn insert(&self, target_id: u32, mounted: MountedNetworkDisk) {
        self.state
            .lock()
            .expect("network_mount_state poisoned")
            .mounted
            .insert(target_id, mounted);
    }

    fn take(&self, target_id: u32) -> Option<MountedNetworkDisk> {
        let mut state = self.state.lock().expect("network_mount_state poisoned");
        state.pending_data_changed_targets.remove(&target_id);
        state.mounted.remove(&target_id)
    }

    fn mark_for_cleanup(&self, target_id: u32) {
        self.state
            .lock()
            .expect("network_mount_state poisoned")
            .pending_cleanup_targets
            .insert(target_id);
    }

    fn mark_session_for_cleanup(&self, addr: &str, session_id: u64) {
        let target_ids = {
            let state = self.state.lock().expect("network_mount_state poisoned");
            state
                .mounted
                .iter()
                .filter_map(|(target_id, mounted)| {
                    if mounted.addr == addr && mounted.session.session_id() == session_id {
                        Some(*target_id)
                    } else {
                        None
                    }
                })
                .collect::<Vec<_>>()
        };

        let mut state = self.state.lock().expect("network_mount_state poisoned");
        for target_id in target_ids {
            state.pending_cleanup_targets.insert(target_id);
        }
    }

    fn mark_connection_for_cleanup(&self, addr: &str) {
        let target_ids = {
            let state = self.state.lock().expect("network_mount_state poisoned");
            state
                .mounted
                .iter()
                .filter_map(|(target_id, mounted)| {
                    if mounted.addr == addr {
                        Some(*target_id)
                    } else {
                        None
                    }
                })
                .collect::<Vec<_>>()
        };

        let mut state = self.state.lock().expect("network_mount_state poisoned");
        for target_id in target_ids {
            state.pending_cleanup_targets.insert(target_id);
        }
    }

    fn mark_session_data_changed(&self, addr: &str, session_id: u64) {
        let target_ids = {
            let state = self.state.lock().expect("network_mount_state poisoned");
            state
                .mounted
                .iter()
                .filter_map(|(target_id, mounted)| {
                    if mounted.addr == addr && mounted.session.session_id() == session_id {
                        Some(*target_id)
                    } else {
                        None
                    }
                })
                .collect::<Vec<_>>()
        };

        let mut state = self.state.lock().expect("network_mount_state poisoned");
        for target_id in target_ids {
            state.pending_data_changed_targets.insert(target_id);
        }
    }

    fn acquire_connection(&self, addr: &str) -> Result<Arc<GatewayConnection>, NetworkClientError> {
        if let Some(connection) = self.connection(addr) {
            return Ok(connection);
        }

        let connection = GatewayConnection::new(TransportEndpoint::new(addr.to_string()));
        let notice_registry = self.clone();
        let notice_addr = addr.to_string();
        connection.set_session_notice_handler(Some(Arc::new(move |notice: SessionCloseNotice| {
            notice_registry.mark_session_for_cleanup(&notice_addr, notice.session_id);
        })));
        let data_changed_registry = self.clone();
        let data_changed_addr = addr.to_string();
        connection.set_session_data_changed_handler(Some(Arc::new(
            move |notice: SessionDataChangedNotice| {
                data_changed_registry
                    .mark_session_data_changed(&data_changed_addr, notice.session_id);
            },
        )));
        let disconnect_registry = self.clone();
        let disconnect_addr = addr.to_string();
        connection.set_disconnect_handler(Some(Arc::new(move || {
            disconnect_registry.mark_connection_for_cleanup(&disconnect_addr);
        })));
        connection.connect()?;

        self.insert_connection(addr.to_string(), Arc::clone(&connection));
        Ok(connection)
    }

    fn connection(&self, addr: &str) -> Option<Arc<GatewayConnection>> {
        let mut state = self.state.lock().expect("network_mount_state poisoned");
        if let Some(connection) = state.connections.get(addr) {
            if connection.is_connected() {
                return Some(Arc::clone(connection));
            }
        }
        state.connections.remove(addr);
        None
    }

    fn insert_connection(&self, addr: String, connection: Arc<GatewayConnection>) {
        self.state
            .lock()
            .expect("network_mount_state poisoned")
            .connections
            .insert(addr, connection);
    }

    fn release_connection_after_session_close(&self, connection: &Arc<GatewayConnection>) {
        let addr = connection.endpoint().address().to_string();
        if connection.should_close_after_session_close() {
            let _ = connection.close();
            self.remove_connection_if_matches(&addr, connection);
            return;
        }
        if !connection.is_connected() {
            self.remove_connection_if_matches(&addr, connection);
        }
    }

    fn remove_connection_if_matches(&self, addr: &str, connection: &Arc<GatewayConnection>) {
        let mut state = self.state.lock().expect("network_mount_state poisoned");
        let should_remove = state
            .connections
            .get(addr)
            .map(|current| Arc::ptr_eq(current, connection))
            .unwrap_or(false);
        if should_remove {
            state.connections.remove(addr);
        }
    }

    fn clear_cleanup_mark(&self, target_id: u32) {
        self.state
            .lock()
            .expect("network_mount_state poisoned")
            .pending_cleanup_targets
            .remove(&target_id);
    }

    fn take_data_changed_target_ids(&self) -> Vec<u32> {
        let mut state = self.state.lock().expect("network_mount_state poisoned");
        let target_ids = state
            .pending_data_changed_targets
            .iter()
            .copied()
            .filter(|target_id| state.mounted.contains_key(target_id))
            .collect::<Vec<_>>();
        for target_id in &target_ids {
            state.pending_data_changed_targets.remove(target_id);
        }
        target_ids
    }

    fn ensure_unique_mount(
        &self,
        addr: &str,
        disk_id: &str,
        remote_backend_id: [u8; 16],
    ) -> AppResult<()> {
        let state = self.state.lock().expect("network_mount_state poisoned");
        for mounted in state.mounted.values() {
            if mounted.addr != addr {
                continue;
            }
            if mounted.disk_id == disk_id {
                return Err("duplicate remote disk".to_string());
            }
            if mounted.remote_backend_id == remote_backend_id {
                return Err("duplicate remote backend".to_string());
            }
        }
        Ok(())
    }

    fn cleanup_marker(&self, target_id: u32) -> Arc<dyn Fn() + Send + Sync> {
        let registry = self.clone();
        Arc::new(move || {
            registry.mark_for_cleanup(target_id);
        })
    }

    fn cleanup_target_ids(&self) -> Vec<u32> {
        let state = self.state.lock().expect("network_mount_state poisoned");
        collect_cleanup_target_ids(&state.mounted, &state.pending_cleanup_targets)
    }

    fn network_mounts(&self) -> Vec<NetworkMountResult> {
        self.state
            .lock()
            .expect("network_mount_state poisoned")
            .mounted
            .iter()
            .map(|(target_id, mounted)| mounted.mount_result(*target_id))
            .collect()
    }

    fn clear(&self) {
        let mut state = self.state.lock().expect("network_mount_state poisoned");
        state.connections.clear();
        state.mounted.clear();
        state.pending_cleanup_targets.clear();
        state.pending_data_changed_targets.clear();
    }
}

fn collect_cleanup_target_ids(
    mounted_network_disks: &BTreeMap<u32, MountedNetworkDisk>,
    pending_cleanup_targets: &BTreeSet<u32>,
) -> Vec<u32> {
    mounted_network_disks
        .iter()
        .filter_map(|(target_id, mounted)| {
            if pending_cleanup_targets.contains(target_id)
                || mounted.session.is_terminal()
                || !mounted.session.is_connection_alive()
            {
                Some(*target_id)
            } else {
                None
            }
        })
        .collect()
}

fn close_session_for_cleanup(session: &DiskSession) -> bool {
    match session.close() {
        Ok(()) | Err(NetworkClientError::SessionUnavailable) => true,
        Err(_) => false,
    }
}

#[cfg(test)]
mod tests {
    use super::MountedNetworkDisk;
    use super::NetworkMountRegistry;
    use super::NetworkMountResult;
    use super::close_session_for_cleanup;
    use super::collect_cleanup_target_ids;
    use network_core::client::DiskSession;
    use network_core::client::GatewayConnection;
    use network_core::test_support::clear_session;
    use network_core::test_support::expect_client_hello;
    use network_core::test_support::is_session_active;
    use network_core::test_support::stage_connection;
    use network_core::transport::TransportEndpoint;
    use std::collections::BTreeMap;
    use std::collections::BTreeSet;
    use std::net::TcpListener;
    use std::sync::Arc;
    use std::thread;
    use std::time::Duration;

    struct ConnectedMountHarness {
        mounted: MountedNetworkDisk,
        connection: Arc<GatewayConnection>,
        server: thread::JoinHandle<()>,
    }

    impl ConnectedMountHarness {
        fn shutdown(self) {
            let _ = self.connection.close();
            let _ = self.server.join();
        }
    }

    fn connected_mount(_target_id: u32, disk_id: &str, session_id: u64) -> ConnectedMountHarness {
        let listener = TcpListener::bind("127.0.0.1:0").expect("bind should succeed");
        let address = listener.local_addr().expect("local addr should succeed");
        let server = thread::spawn(move || {
            let (mut stream, _) = listener.accept().expect("accept should succeed");
            expect_client_hello(&mut stream);
            thread::sleep(Duration::from_millis(200));
        });

        let connection = stage_connection(TransportEndpoint::new(address.to_string()), session_id);
        connection.connect().expect("connect should succeed");
        let session =
            DiskSession::new(connection.clone(), session_id).expect("session should build");
        ConnectedMountHarness {
            mounted: MountedNetworkDisk {
                addr: address.to_string(),
                disk_id: disk_id.to_string(),
                remote_backend_id: [0u8; 16],
                session,
                disk_size_bytes: 4096,
                read_only: false,
            },
            connection,
            server,
        }
    }

    #[test]
    fn collect_cleanup_target_ids_includes_explicit_pending_targets() {
        let mount = connected_mount(3, "A1b2C3d4E5f6G7h8", 77);
        let mounted_network_disks = BTreeMap::from([(3, mount.mounted.clone())]);
        let pending_cleanup_targets = BTreeSet::from([3]);

        assert_eq!(
            collect_cleanup_target_ids(&mounted_network_disks, &pending_cleanup_targets),
            vec![3]
        );

        mount.shutdown();
    }

    #[test]
    fn collect_cleanup_target_ids_includes_connection_loss() {
        let connection = stage_connection(TransportEndpoint::new("127.0.0.1:1"), 77);
        let session = DiskSession::new(connection, 77).expect("session should build");
        let mounted_network_disks = BTreeMap::from([(
            4,
            MountedNetworkDisk {
                addr: "127.0.0.1:1".to_string(),
                disk_id: "A1b2C3d4E5f6G7h8".to_string(),
                remote_backend_id: [0u8; 16],
                session,
                disk_size_bytes: 4096,
                read_only: false,
            },
        )]);

        assert_eq!(
            collect_cleanup_target_ids(&mounted_network_disks, &BTreeSet::new()),
            vec![4]
        );
    }

    #[test]
    fn collect_cleanup_target_ids_includes_terminal_sessions() {
        let mount = connected_mount(5, "A1b2C3d4E5f6G7h8", 88);
        clear_session(&mount.connection, 88);
        let _ = mount
            .mounted
            .session
            .ensure_usable()
            .expect_err("cleared session should fail");
        let mounted_network_disks = BTreeMap::from([(5, mount.mounted.clone())]);

        assert_eq!(
            collect_cleanup_target_ids(&mounted_network_disks, &BTreeSet::new()),
            vec![5]
        );

        mount.shutdown();
    }

    #[test]
    fn network_mount_registry_tracks_mounts_and_cleanup_marks() {
        let mount = connected_mount(6, "A1b2C3d4E5f6G7h8", 99);
        let registry = NetworkMountRegistry::default();
        registry.insert(6, mount.mounted.clone());

        assert_eq!(
            registry.network_mounts(),
            vec![NetworkMountResult {
                target_id: 6,
                addr: mount.mounted.addr.clone(),
                disk_id: mount.mounted.disk_id.clone(),
                session_id: 99,
                disk_size_bytes: 4096,
                read_only: false,
            }]
        );

        registry.mark_for_cleanup(6);
        assert_eq!(registry.cleanup_target_ids(), vec![6]);

        let taken = registry.take(6).expect("mount should exist");
        assert_eq!(taken.session.session_id(), 99);
        registry.clear_cleanup_mark(6);
        assert!(registry.network_mounts().is_empty());

        mount.shutdown();
    }

    #[test]
    fn network_mount_registry_rejects_duplicate_remote_disk_and_backend() {
        let mount = connected_mount(7, "A1b2C3d4E5f6G7h8", 100);
        let registry = NetworkMountRegistry::default();
        registry.insert(7, mount.mounted.clone());

        let duplicate_disk = registry
            .ensure_unique_mount(&mount.mounted.addr, &mount.mounted.disk_id, [1u8; 16])
            .expect_err("duplicate disk should fail");
        assert_eq!(duplicate_disk, "duplicate remote disk");

        let duplicate_backend = registry
            .ensure_unique_mount(&mount.mounted.addr, "B1b2C3d4E5f6G7h8", [0u8; 16])
            .expect_err("duplicate backend should fail");
        assert_eq!(duplicate_backend, "duplicate remote backend");

        let other_addr =
            registry.ensure_unique_mount("127.0.0.1:2", &mount.mounted.disk_id, [0u8; 16]);
        assert!(other_addr.is_ok());

        mount.shutdown();
    }

    #[test]
    fn network_mount_registry_tracks_pending_data_changed_targets() {
        let mount = connected_mount(8, "A1b2C3d4E5f6G7h8", 101);
        let registry = NetworkMountRegistry::default();
        registry.insert(8, mount.mounted.clone());

        registry.mark_session_data_changed(&mount.mounted.addr, 101);
        assert_eq!(registry.take_data_changed_target_ids(), vec![8]);
        assert!(registry.take_data_changed_target_ids().is_empty());

        mount.shutdown();
    }

    #[test]
    fn close_session_for_cleanup_marks_cleared_session_as_closed() {
        let connection = stage_connection(TransportEndpoint::new("127.0.0.1:1"), 123);
        let session = DiskSession::new(connection.clone(), 123).expect("session should build");

        clear_session(&connection, 123);

        assert!(close_session_for_cleanup(&session));
        assert!(session.is_closed());
        assert!(!is_session_active(&connection, 123));
    }
}
