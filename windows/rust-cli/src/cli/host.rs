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

pub struct CliHost {
    context: BackendContext,
    mounted_network_disks: Arc<Mutex<BTreeMap<u32, MountedNetworkDisk>>>,
    pending_cleanup_targets: Arc<Mutex<BTreeSet<u32>>>,
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

        let mounted_network_disks = Arc::new(Mutex::new(BTreeMap::new()));
        let pending_cleanup_targets = Arc::new(Mutex::new(BTreeSet::new()));

        Ok(Self {
            context,
            mounted_network_disks,
            pending_cleanup_targets,
        })
    }

    pub fn shutdown(&mut self) {
        self.remove_all_disks_for_shutdown();
        self.context.close();
        self.mounted_network_disks
            .lock()
            .expect("mounted_network_disks poisoned")
            .clear();
        self.pending_cleanup_targets
            .lock()
            .expect("pending_cleanup_targets poisoned")
            .clear();
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

        let connection = GatewayConnection::new(TransportEndpoint::new(addr.to_string()));
        connection.connect().map_err(|error| error.to_string())?;
        let authenticator = ConnectionAuthenticator::new(Arc::clone(&connection));
        let auth = authenticator
            .authenticate(claim_code)
            .map_err(|error| error.to_string())?;

        let opener = SessionOpener::new(Arc::clone(&connection));
        let session_id = opener.open(&auth).map_err(|error| {
            let _ = connection.close();
            error.to_string()
        })?;
        let session = DiskSession::new(Arc::clone(&connection), session_id).map_err(|error| {
            let _ = connection.close();
            error.to_string()
        })?;
        let describer = SessionDescriber::new(Arc::clone(&connection));
        let metadata = describer.describe(session_id).map_err(|error| {
            let _ = session.close();
            let _ = connection.close();
            error.to_string()
        })?;

        let pending_cleanup_targets = Arc::clone(&self.pending_cleanup_targets);
        let media = NetworkMedia::bind(auth.disk_id().to_string(), session.clone(), metadata)
            .map_err(|error| {
                let _ = session.close();
                let _ = connection.close();
                error.to_string()
            })?
            .with_invalidation_handler(Arc::new(move || {
                pending_cleanup_targets
                    .lock()
                    .expect("pending_cleanup_targets poisoned")
                    .insert(target_id);
            }));

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
            let _ = session.close();
            let _ = connection.close();
            if error_text.is_empty() {
                error_text = "create-failed".to_string();
            }
            return Err(error_text);
        }

        let pending_cleanup_targets = Arc::clone(&self.pending_cleanup_targets);
        connection.set_session_notice_handler(Some(Arc::new(
            move |_notice: SessionCloseNotice| {
                pending_cleanup_targets
                    .lock()
                    .expect("pending_cleanup_targets poisoned")
                    .insert(target_id);
            },
        )));
        let pending_cleanup_targets = Arc::clone(&self.pending_cleanup_targets);
        connection.set_disconnect_handler(Some(Arc::new(move || {
            pending_cleanup_targets
                .lock()
                .expect("pending_cleanup_targets poisoned")
                .insert(target_id);
        })));

        self.mounted_network_disks
            .lock()
            .expect("mounted_network_disks poisoned")
            .insert(
                target_id,
                MountedNetworkDisk {
                    addr: addr.to_string(),
                    disk_id: auth.disk_id().to_string(),
                    session: session.clone(),
                    disk_size_bytes: metadata.disk_size_bytes,
                    read_only: metadata.read_only,
                    max_io_bytes: metadata.max_io_bytes,
                },
            );

        Ok(NetworkMountResult {
            target_id,
            addr: addr.to_string(),
            disk_id: auth.disk_id().to_string(),
            session_id: session.session_id(),
            disk_size_bytes: metadata.disk_size_bytes,
            read_only: metadata.read_only,
            max_io_bytes: metadata.max_io_bytes,
        })
    }

    pub fn remove_disk(&mut self, target_id: u32) -> AppResult<()> {
        if let Some(mounted) = self
            .mounted_network_disks
            .lock()
            .expect("mounted_network_disks poisoned")
            .get(&target_id)
            .cloned()
        {
            mounted
                .session
                .connection()
                .set_session_notice_handler(None);
            mounted.session.connection().set_disconnect_handler(None);
            match mounted.session.close() {
                Ok(()) => {}
                Err(NetworkClientError::SessionUnavailable) => {}
                Err(error) => return Err(error.to_string()),
            }
            let _ = mounted.session.connection().close();
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

        self.mounted_network_disks
            .lock()
            .expect("mounted_network_disks poisoned")
            .remove(&target_id);
        self.pending_cleanup_targets
            .lock()
            .expect("pending_cleanup_targets poisoned")
            .remove(&target_id);

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
            if let Some(mounted) = self
                .mounted_network_disks
                .lock()
                .expect("mounted_network_disks poisoned")
                .remove(&target_id)
            {
                mounted
                    .session
                    .connection()
                    .set_session_notice_handler(None);
                mounted.session.connection().set_disconnect_handler(None);
                let _ = mounted.session.connection().close();
            }
            self.pending_cleanup_targets
                .lock()
                .expect("pending_cleanup_targets poisoned")
                .remove(&target_id);
        }
    }

    pub fn reap_dead_network_disks(&mut self) -> AppResult<Vec<u32>> {
        let pending_targets = self
            .pending_cleanup_targets
            .lock()
            .expect("pending_cleanup_targets poisoned")
            .clone();
        let removals = {
            let mounted_network_disks = self
                .mounted_network_disks
                .lock()
                .expect("mounted_network_disks poisoned");
            collect_cleanup_target_ids(&mounted_network_disks, &pending_targets)
        };

        let mut removed = Vec::new();
        for target_id in &removals {
            self.remove_disk(*target_id)?;
            removed.push(*target_id);
        }
        let mut pending = self
            .pending_cleanup_targets
            .lock()
            .expect("pending_cleanup_targets poisoned");
        for target_id in removals {
            pending.remove(&target_id);
        }
        Ok(removed)
    }

    pub fn network_mounts(&self) -> Vec<NetworkMountResult> {
        self.mounted_network_disks
            .lock()
            .expect("mounted_network_disks poisoned")
            .iter()
            .map(|(target_id, mounted)| NetworkMountResult {
                target_id: *target_id,
                addr: mounted.addr.clone(),
                disk_id: mounted.disk_id.clone(),
                session_id: mounted.session.session_id(),
                disk_size_bytes: mounted.disk_size_bytes,
                read_only: mounted.read_only,
                max_io_bytes: mounted.max_io_bytes,
            })
            .collect()
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

#[cfg(test)]
mod tests {
    use super::MountedNetworkDisk;
    use super::collect_cleanup_target_ids;
    use crate::network::DiskSession;
    use crate::network::GatewayConnection;
    use crate::network::TransportEndpoint;
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
            let (_stream, _) = listener.accept().expect("accept should succeed");
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
}
