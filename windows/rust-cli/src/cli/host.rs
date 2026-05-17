use std::collections::BTreeMap;
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
}

pub struct CliHost {
    context: BackendContext,
    mounted_network_disks: Arc<Mutex<BTreeMap<u32, MountedNetworkDisk>>>,
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

        Ok(Self {
            context,
            mounted_network_disks,
        })
    }

    pub fn shutdown(&mut self) {
        self.remove_all_disks_for_shutdown();
        self.context.close();
        self.mounted_network_disks
            .lock()
            .expect("mounted_network_disks poisoned")
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
        let connection = GatewayConnection::new(TransportEndpoint::new(addr.to_string()));
        connection.connect().map_err(|error| error.to_string())?;
        let authenticator = ConnectionAuthenticator::new(Arc::clone(&connection));
        let disk_id = authenticator
            .authenticate(claim_code)
            .map_err(|error| error.to_string())?;

        let opener = SessionOpener::new(Arc::clone(&connection));
        let session = opener
            .open(disk_id.clone())
            .map_err(|error| {
                let _ = connection.close();
                error.to_string()
            })?;

        let media = NetworkMedia::bind(
            session.clone(),
            session.disk_size_bytes(),
            session.read_only(),
            session.max_io_bytes(),
        )
        .map_err(|error| {
            let _ = session.close();
            let _ = connection.close();
            error.to_string()
        })?;

        let target_id = target_id.unwrap_or_else(|| self.context.find_first_free_target());
        if target_id > YUMEDISK_MAX_USABLE_TARGET_ID {
            return Err("no-free-target".to_string());
        }

        let disk_config = DiskConfig {
            target_id,
            disk_size_bytes: session.disk_size_bytes(),
            read_only: session.read_only(),
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

        self.mounted_network_disks
            .lock()
            .expect("mounted_network_disks poisoned")
            .insert(
                target_id,
                MountedNetworkDisk {
                    addr: addr.to_string(),
                    disk_id: disk_id.clone(),
                    session: session.clone(),
                },
            );

        Ok(NetworkMountResult {
            target_id,
            addr: addr.to_string(),
            disk_id,
            session_id: session.session_id(),
            disk_size_bytes: session.disk_size_bytes(),
            read_only: session.read_only(),
            max_io_bytes: session.max_io_bytes(),
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
                let _ = mounted.session.connection().close();
            }
        }
    }

    pub fn reap_dead_network_disks(&mut self) -> AppResult<Vec<u32>> {
        let target_ids = self
            .mounted_network_disks
            .lock()
            .expect("mounted_network_disks poisoned")
            .iter()
            .filter_map(|(target_id, mounted)| {
                if mounted.session.is_terminal() {
                    Some(*target_id)
                } else {
                    None
                }
            })
            .collect::<Vec<_>>();

        let mut removed = Vec::new();
        for target_id in target_ids {
            self.remove_disk(target_id)?;
            removed.push(target_id);
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
                disk_size_bytes: mounted.session.disk_size_bytes(),
                read_only: mounted.session.read_only(),
                max_io_bytes: mounted.session.max_io_bytes(),
            })
            .collect()
    }
}
