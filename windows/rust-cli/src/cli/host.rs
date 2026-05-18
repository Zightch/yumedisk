use std::collections::BTreeMap;
use std::collections::BTreeSet;
use std::sync::Arc;
use std::sync::Mutex;

use backend_rust::BackendContext;
use backend_rust::BackendStatsSnapshot;
use backend_rust::DebugSnapshot;
use backend_rust::DiskConfig;
use backend_rust::ManagedDiskSnapshot;
use backend_rust::Media;
use backend_rust::YUMEDISK_MAX_TARGETS;
use backend_rust::YUMEDISK_MAX_USABLE_TARGET_ID;

use crate::cli::local::DenseMem;
use crate::network::ConnectionAuthenticator;
use crate::network::DiskSession;
use crate::network::GatewayConnection;
use crate::network::NetworkClientError;
use crate::network::NetworkMedia;
use crate::network::SessionCloseNotice;
use crate::network::SessionDescriber;
use crate::network::SessionOpener;
use crate::network::TransportEndpoint;

pub type AppResult<T> = Result<T, String>;

#[derive(Debug, Clone, Copy)]
pub struct CreateDenseDiskRequest {
    pub disk_size_mib: u64,
    pub read_only: bool,
    pub target_id: Option<u32>,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct NetworkMountResult {
    pub target_id: u32,
    pub addr: String,
    pub disk_id: String,
    pub session_id: u64,
    pub disk_size_bytes: u64,
    pub read_only: bool,
    pub max_io_bytes: u32,
}

#[derive(Debug, Clone)]
struct MountedNetworkDisk {
    addr: String,
    disk_id: String,
    session: DiskSession,
    disk_size_bytes: u64,
    read_only: bool,
    max_io_bytes: u32,
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
}

pub struct CliHost {
    context: BackendContext,
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

    pub fn create_dense_disk(&mut self, request: CreateDenseDiskRequest) -> AppResult<u32> {
        let disk_size_bytes = request
            .disk_size_mib
            .checked_mul(1024 * 1024)
            .ok_or_else(|| "disk-size-overflow".to_string())?;
        let target_id = request
            .target_id
            .unwrap_or_else(|| self.context.find_first_free_target());
        if target_id >= YUMEDISK_MAX_TARGETS {
            return Err("no-free-target".to_string());
        }

        let dense_mem = DenseMem::new(disk_size_bytes).map_err(|error| error.to_string())?;
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
            session.clone(),
            metadata.disk_size_bytes,
            metadata.read_only,
            metadata.max_io_bytes,
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
        let mut session_closed = false;
        if let Some(mounted) = self.network_mounts.mounted(target_id) {
            session_closed = mounted.close_for_remove();
        }

        let mut error_text = String::new();
        let media = self
            .context
            .remove_managed_disk_with_media(target_id, Some(&mut error_text));
        let Some(_media) = media else {
            if error_text.is_empty() {
                error_text = "remove-failed".to_string();
            }
            return Err(error_text);
        };

        if let Some(mounted) = self.network_mounts.take(target_id) {
            if session_closed {
                self.network_mounts
                    .release_connection_after_session_close(mounted.session.connection());
            }
        }
        self.network_mounts.clear_cleanup_mark(target_id);

        Ok(())
    }

    pub fn remove_all_disks(&mut self) -> AppResult<()> {
        let target_ids = self
            .context
            .snapshot_managed_disks()
            .into_iter()
            .map(|disk| disk.target_id)
            .collect::<Vec<_>>();

        for target_id in target_ids {
            self.remove_disk(target_id)?;
        }
        Ok(())
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
            let _ = self
                .context
                .remove_managed_disk_with_media(target_id, Some(&mut error_text));
            if let Some(mounted) = self.network_mounts.take(target_id) {
                mounted.shutdown();
            }
            self.network_mounts.clear_cleanup_mark(target_id);
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
}

impl MountedNetworkDisk {
    fn new(
        addr: String,
        disk_id: String,
        session: DiskSession,
        disk_size_bytes: u64,
        read_only: bool,
        max_io_bytes: u32,
    ) -> Self {
        Self {
            addr,
            disk_id,
            session,
            disk_size_bytes,
            read_only,
            max_io_bytes,
        }
    }

    fn close_for_remove(&self) -> bool {
        close_session_for_cleanup(&self.session)
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
            max_io_bytes: self.max_io_bytes,
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

    fn mounted(&self, target_id: u32) -> Option<MountedNetworkDisk> {
        self.state
            .lock()
            .expect("network_mount_state poisoned")
            .mounted
            .get(&target_id)
            .cloned()
    }

    fn take(&self, target_id: u32) -> Option<MountedNetworkDisk> {
        self.state
            .lock()
            .expect("network_mount_state poisoned")
            .mounted
            .remove(&target_id)
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
    use super::close_session_for_cleanup;
    use super::MountedNetworkDisk;
    use super::NetworkMountRegistry;
    use super::NetworkMountResult;
    use super::collect_cleanup_target_ids;
    use crate::network::DiskSession;
    use crate::network::GatewayConnection;
    use crate::network::TransportEndpoint;
    use crate::network::expect_client_hello;
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

    fn staged_connection(endpoint: TransportEndpoint, session_id: u64) -> Arc<GatewayConnection> {
        let connection = GatewayConnection::new(endpoint);
        connection
            .begin_session_open()
            .expect("begin session open should succeed");
        connection
            .finish_session_open(session_id)
            .expect("finish session open should succeed");
        connection
    }

    fn connected_mount(_target_id: u32, disk_id: &str, session_id: u64) -> ConnectedMountHarness {
        let listener = TcpListener::bind("127.0.0.1:0").expect("bind should succeed");
        let address = listener.local_addr().expect("local addr should succeed");
        let server = thread::spawn(move || {
            let (mut stream, _) = listener.accept().expect("accept should succeed");
            expect_client_hello(&mut stream);
            thread::sleep(Duration::from_millis(200));
        });

        let connection = staged_connection(TransportEndpoint::new(address.to_string()), session_id);
        connection.connect().expect("connect should succeed");
        let session =
            DiskSession::new(connection.clone(), session_id).expect("session should build");
        ConnectedMountHarness {
            mounted: MountedNetworkDisk {
                addr: address.to_string(),
                disk_id: disk_id.to_string(),
                session,
                disk_size_bytes: 4096,
                read_only: false,
                max_io_bytes: 4096,
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
        let connection = staged_connection(TransportEndpoint::new("127.0.0.1:1"), 77);
        let session = DiskSession::new(connection, 77).expect("session should build");
        let mounted_network_disks = BTreeMap::from([(
            4,
            MountedNetworkDisk {
                addr: "127.0.0.1:1".to_string(),
                disk_id: "A1b2C3d4E5f6G7h8".to_string(),
                session,
                disk_size_bytes: 4096,
                read_only: false,
                max_io_bytes: 4096,
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
        mount.connection.clear_session(88);
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
                max_io_bytes: 4096,
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
    fn close_session_for_cleanup_marks_cleared_session_as_closed() {
        let connection = staged_connection(TransportEndpoint::new("127.0.0.1:1"), 123);
        let session = DiskSession::new(connection.clone(), 123).expect("session should build");

        connection.clear_session(123);

        assert!(close_session_for_cleanup(&session));
        assert!(session.is_closed());
        assert!(!connection.is_session_active(123));
    }
}
