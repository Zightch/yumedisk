use std::collections::BTreeMap;
use std::ffi::c_void;
use std::sync::Arc;
use std::sync::Mutex;
use std::sync::RwLock;
use std::sync::atomic::AtomicBool;
use std::sync::atomic::Ordering;
use std::thread::JoinHandle;

use crate::appkernel;
use crate::config;
use crate::error::BackendError;
use crate::media::Media;
use crate::scan;
use crate::staging::StagingStore;
use crate::types;
use crate::win32;

fn format_status_hex(status: appkernel::AkStatus) -> String {
    format!("0x{:08X}", status as u32)
}

fn format_version_be(version_be: u32) -> String {
    format!(
        "{}.{}.{}.{}",
        (version_be >> 24) & 0xff,
        (version_be >> 16) & 0xff,
        (version_be >> 8) & 0xff,
        version_be & 0xff
    )
}

fn lifecycle_to_text(lifecycle: appkernel::AkLifecycleState) -> &'static str {
    match lifecycle {
        appkernel::AkLifecycleState::Init => "init",
        appkernel::AkLifecycleState::Starting => "starting",
        appkernel::AkLifecycleState::Running => "running",
        appkernel::AkLifecycleState::Removing => "removing",
        appkernel::AkLifecycleState::Closing => "closing",
        appkernel::AkLifecycleState::Closed => "closed",
        appkernel::AkLifecycleState::Broken => "broken",
    }
}

fn wide_from_multibyte(text: *const i8, code_page: u32) -> String {
    if text.is_null() {
        return String::new();
    }

    // SAFETY: Windows API length query.
    let length =
        unsafe { win32::MultiByteToWideChar(code_page, 0, text, -1, core::ptr::null_mut(), 0) };
    if length <= 1 {
        return String::new();
    }

    let mut buffer = vec![0u16; length as usize];
    // SAFETY: allocated target buffer.
    let written =
        unsafe { win32::MultiByteToWideChar(code_page, 0, text, -1, buffer.as_mut_ptr(), length) };
    if written <= 1 {
        return String::new();
    }

    if let Some(&0) = buffer.last() {
        buffer.pop();
    }
    String::from_utf16_lossy(&buffer)
}

fn wide_from_text(text: *const i8) -> String {
    let result = wide_from_multibyte(text, win32::CP_UTF8);
    if !result.is_empty() {
        return result;
    }
    wide_from_multibyte(text, win32::CP_ACP)
}

fn read_only_to_text(read_only: bool) -> &'static str {
    if read_only { "true" } else { "false" }
}

fn append_log_inner(inner: &Inner, text: String) {
    {
        let mut log_lines = inner.log_lines.lock().expect("log_lines poisoned");
        if log_lines.len() >= types::MAX_BUFFERED_LOG_LINES {
            log_lines.remove(0);
        }
        log_lines.push(text.clone());
    }

    let mut wide_text: Vec<u16> = text.encode_utf16().collect();
    wide_text.push('\n' as u16);
    wide_text.push(0);
    // SAFETY: null-terminated UTF-16 buffer.
    unsafe { win32::OutputDebugStringW(wide_text.as_ptr()) };
}

#[derive(Default)]
struct DiskLifecycleState {
    handle: usize,
}

struct DiskMediaState {
    instance: Option<Box<dyn Media>>,
}

struct DiskRuntime {
    metadata: types::DiskMetadata,
    queue_config: types::DiskQueueConfig,
    lifecycle: DiskLifecycleState,
    media: DiskMediaState,
    staging: StagingStore,
}

impl DiskRuntime {
    fn new(disk_config: &types::DiskConfig) -> Self {
        Self {
            metadata: types::DiskMetadata {
                target_id: disk_config.target_id,
                sector_size: disk_config.sector_size,
                disk_size_bytes: disk_config.disk_size_bytes,
                read_only: disk_config.read_only,
                identity: scan::DiskIdentity::default(),
            },
            queue_config: types::DiskQueueConfig {
                queue_depth: disk_config.queue_depth,
                write_slot_bytes: disk_config.write_slot_bytes,
                read_worker_count: disk_config.read_worker_count,
                write_worker_count: disk_config.write_worker_count,
                ack_batch_max_ranges: disk_config.ack_batch_max_ranges,
            },
            lifecycle: DiskLifecycleState::default(),
            media: DiskMediaState { instance: None },
            staging: StagingStore::new(),
        }
    }
}

struct Inner {
    session_config: Mutex<types::SessionConfig>,
    session: Mutex<usize>,
    stop_event: Mutex<usize>,
    stop: AtomicBool,
    disk_runtimes: Mutex<BTreeMap<u32, Arc<RwLock<DiskRuntime>>>>,
    log_lines: Mutex<Vec<String>>,
    event_thread: Mutex<Option<JoinHandle<()>>>,
    open_status: Mutex<appkernel::AkStatus>,
    open_win32_error: Mutex<u32>,
    open_succeeded: Mutex<bool>,
}

pub struct BackendContext {
    inner: Arc<Inner>,
}

impl Default for BackendContext {
    fn default() -> Self {
        Self::new()
    }
}

impl Drop for BackendContext {
    fn drop(&mut self) {
        self.close();
    }
}

impl BackendContext {
    pub fn new() -> Self {
        Self {
            inner: Arc::new(Inner {
                session_config: Mutex::new(types::SessionConfig::default()),
                session: Mutex::new(0),
                stop_event: Mutex::new(0),
                stop: AtomicBool::new(false),
                disk_runtimes: Mutex::new(BTreeMap::new()),
                log_lines: Mutex::new(Vec::new()),
                event_thread: Mutex::new(None),
                open_status: Mutex::new(appkernel::AK_STATUS_SUCCESS),
                open_win32_error: Mutex::new(win32::ERROR_SUCCESS),
                open_succeeded: Mutex::new(false),
            }),
        }
    }

    fn snapshot_disk_runtimes(&self) -> Vec<Arc<RwLock<DiskRuntime>>> {
        self.inner
            .disk_runtimes
            .lock()
            .expect("disk map poisoned")
            .values()
            .cloned()
            .collect()
    }

    fn find_disk_runtime(&self, target_id: u32) -> Option<Arc<RwLock<DiskRuntime>>> {
        self.inner
            .disk_runtimes
            .lock()
            .expect("disk map poisoned")
            .get(&target_id)
            .cloned()
    }

    fn insert_disk_runtime(&self, disk_runtime: Arc<RwLock<DiskRuntime>>) {
        let target_id = disk_runtime
            .read()
            .expect("disk runtime poisoned")
            .metadata
            .target_id;
        self.inner
            .disk_runtimes
            .lock()
            .expect("disk map poisoned")
            .insert(target_id, disk_runtime);
    }

    fn erase_disk_runtime(&self, target_id: u32) {
        self.inner
            .disk_runtimes
            .lock()
            .expect("disk map poisoned")
            .remove(&target_id);
    }

    fn disk_runtime_exists(&self, target_id: u32) -> bool {
        self.inner
            .disk_runtimes
            .lock()
            .expect("disk map poisoned")
            .contains_key(&target_id)
    }

    fn snapshot_claimed_disk_paths(&self, excluded_target_id: u32) -> Vec<String> {
        self.inner
            .disk_runtimes
            .lock()
            .expect("disk map poisoned")
            .iter()
            .filter_map(|(target_id, runtime)| {
                if *target_id == excluded_target_id {
                    return None;
                }
                let runtime = runtime.read().expect("disk runtime poisoned");
                if runtime.metadata.identity.path.is_empty() {
                    None
                } else {
                    Some(runtime.metadata.identity.path.clone())
                }
            })
            .collect()
    }

    fn try_refresh_disk_runtime_identity(
        &self,
        disk_runtime: &Arc<RwLock<DiskRuntime>>,
        baseline_visible_disks: Option<&[scan::DiskIdentity]>,
        timeout_ms: u32,
    ) -> bool {
        let excluded_target_id = disk_runtime
            .read()
            .expect("disk runtime poisoned")
            .metadata
            .target_id;
        let claimed_paths = self.snapshot_claimed_disk_paths(excluded_target_id);
        // SAFETY: Win32 tick count.
        let start_tick = unsafe { win32::GetTickCount64() };

        loop {
            let visible_disks = scan::enumerate_visible_yumedisks();
            let mut selected = None;

            for identity in &visible_disks {
                if claimed_paths.iter().any(|path| path == &identity.path) {
                    continue;
                }
                if baseline_visible_disks
                    .map(|baseline| baseline.iter().any(|item| item.path == identity.path))
                    .unwrap_or(false)
                {
                    continue;
                }

                selected = Some(identity.clone());
                break;
            }

            if selected.is_none() && !visible_disks.is_empty() {
                for identity in &visible_disks {
                    if !claimed_paths.iter().any(|path| path == &identity.path) {
                        selected = Some(identity.clone());
                        break;
                    }
                }
            }

            if let Some(identity) = selected {
                disk_runtime
                    .write()
                    .expect("disk runtime poisoned")
                    .metadata
                    .identity = identity;
                return true;
            }

            // SAFETY: Win32 tick count.
            let elapsed = unsafe { win32::GetTickCount64() - start_tick };
            if timeout_ms == 0
                || self.inner.stop.load(Ordering::Relaxed)
                || elapsed >= u64::from(timeout_ms)
            {
                return false;
            }

            // SAFETY: sleep.
            unsafe { win32::Sleep(types::DISK_ARRIVAL_POLL_MS) };
        }
    }

    fn make_managed_disk_snapshot(
        &self,
        disk_runtime: &Arc<RwLock<DiskRuntime>>,
    ) -> types::ManagedDiskSnapshot {
        let _ = self.try_refresh_disk_runtime_identity(disk_runtime, None, 0);
        let runtime = disk_runtime.read().expect("disk runtime poisoned");
        let mut snapshot = types::ManagedDiskSnapshot {
            target_id: runtime.metadata.target_id,
            disk_size_bytes: runtime.metadata.disk_size_bytes,
            sector_size: runtime.metadata.sector_size,
            read_only: runtime.metadata.read_only,
            visible_path: runtime.metadata.identity.path.clone(),
            physical_drive_path: scan::make_physical_drive_path(
                runtime.metadata.identity.device_number,
            ),
            lifecycle_text: String::from("unknown"),
            online: false,
        };

        if runtime.lifecycle.handle != 0 {
            let mut disk_state = appkernel::AkDiskState {
                lifecycle: appkernel::AkLifecycleState::Init,
                target_id: 0,
                disk_runtime_id: 0,
                read_workers_running: 0,
                write_workers_running: 0,
                ack_flusher_running: 0,
                last_error: 0,
            };
            // SAFETY: valid AppKernel handle while runtime exists.
            let status = unsafe {
                appkernel::AkQueryDiskState(
                    runtime.lifecycle.handle as *mut appkernel::AkDisk,
                    &mut disk_state,
                )
            };
            if status == appkernel::AK_STATUS_SUCCESS {
                snapshot.online =
                    matches!(disk_state.lifecycle, appkernel::AkLifecycleState::Running);
                snapshot.lifecycle_text = lifecycle_to_text(disk_state.lifecycle).to_string();
            }
        }

        snapshot
    }

    fn handle_appkernel_event(&self, event_record: &appkernel::AkEvent) {
        if matches!(
            event_record.event_type,
            appkernel::AkEventType::SessionBroken
        ) {
            self.append_log(format!(
                "[backend] session broken, status={}",
                format_status_hex(event_record.status)
            ));
            self.inner.stop.store(true, Ordering::Relaxed);
            let stop_event = *self.inner.stop_event.lock().expect("stop_event poisoned");
            if stop_event != 0 {
                // SAFETY: event handle created by CreateEventW.
                unsafe { win32::SetEvent(stop_event as win32::Handle) };
            }
            return;
        }

        let Some(disk_runtime) = self.find_disk_runtime(event_record.target_id) else {
            return;
        };

        match event_record.event_type {
            appkernel::AkEventType::DiskOnline => {
                let _ = self.try_refresh_disk_runtime_identity(&disk_runtime, None, 0);
            }
            appkernel::AkEventType::WriteFinalCommitted => {
                if !commit_disk_runtime_staging(&disk_runtime, event_record.event_id) {
                    self.append_log(format!(
                        "[backend] commit write failed, target={}, event={}",
                        event_record.target_id, event_record.event_id
                    ));
                }
            }
            appkernel::AkEventType::WriteFinalRejected => {
                reject_disk_runtime_staging(&disk_runtime, event_record.event_id);
            }
            appkernel::AkEventType::DiskRemoved | appkernel::AkEventType::SessionBroken => {}
        }
    }

    fn run_event_loop(self: Arc<Self>) {
        loop {
            if self.inner.stop.load(Ordering::Relaxed) {
                break;
            }

            let session = *self.inner.session.lock().expect("session poisoned");
            if session == 0 {
                break;
            }

            let mut event_record = appkernel::AkEvent {
                event_type: appkernel::AkEventType::DiskOnline,
                target_id: 0,
                disk_runtime_id: 0,
                event_id: 0,
                total_seq: 0,
                flags: 0,
                status: 0,
            };

            // SAFETY: valid session while open.
            let status = unsafe {
                appkernel::AkPollEvent(session as *mut appkernel::AkSession, &mut event_record)
            };
            if status == appkernel::AK_STATUS_NO_MORE_ENTRIES {
                // SAFETY: bounded polling sleep.
                unsafe { win32::Sleep(types::EVENT_WAIT_POLL_MS) };
                continue;
            }
            if status != appkernel::AK_STATUS_SUCCESS {
                self.append_log(format!(
                    "[backend] event loop failed, status={}",
                    format_status_hex(status)
                ));
                self.inner.stop.store(true, Ordering::Relaxed);
                let stop_event = *self.inner.stop_event.lock().expect("stop_event poisoned");
                if stop_event != 0 {
                    // SAFETY: event handle created by CreateEventW.
                    unsafe { win32::SetEvent(stop_event as win32::Handle) };
                }
                break;
            }

            self.handle_appkernel_event(&event_record);
        }
    }

    fn discard_all_disk_runtime_state(&self) {
        let runtime_list = self.snapshot_disk_runtimes();
        for disk_runtime in runtime_list {
            let mut runtime = disk_runtime.write().expect("disk runtime poisoned");
            runtime.lifecycle.handle = 0;
            runtime.staging.clear_locked();
            runtime.media.instance = None;
        }
        self.inner
            .disk_runtimes
            .lock()
            .expect("disk map poisoned")
            .clear();
    }

    pub fn set_session_config(
        &self,
        session_config: types::SessionConfig,
    ) -> Result<(), BackendError> {
        let session = *self.inner.session.lock().expect("session poisoned");
        if session != 0 {
            self.append_log("[backend] ignore session config update while open".to_string());
            return Ok(());
        }

        *self
            .inner
            .session_config
            .lock()
            .expect("session_config poisoned") = session_config;
        Ok(())
    }

    pub fn session_config(&self) -> types::SessionConfig {
        self.inner
            .session_config
            .lock()
            .expect("session_config poisoned")
            .clone()
    }

    pub fn open(&self) -> bool {
        let mut session_guard = self.inner.session.lock().expect("session poisoned");
        if *session_guard != 0 {
            return true;
        }

        self.inner.stop.store(false, Ordering::Relaxed);
        *self.inner.open_status.lock().expect("open_status poisoned") =
            appkernel::AK_STATUS_SUCCESS;
        *self
            .inner
            .open_win32_error
            .lock()
            .expect("open_win32_error poisoned") = win32::ERROR_SUCCESS;
        *self
            .inner
            .open_succeeded
            .lock()
            .expect("open_succeeded poisoned") = false;

        let session_config = self.session_config();
        if let Err(error) = config::validate_session_config(&session_config) {
            *self.inner.open_status.lock().expect("open_status poisoned") =
                appkernel::AK_STATUS_INVALID_PARAMETER;
            *self
                .inner
                .open_win32_error
                .lock()
                .expect("open_win32_error poisoned") = win32::ERROR_INVALID_PARAMETER;
            self.append_log(format!(
                "[backend] invalid session config, reason={}",
                error.as_code()
            ));
            return false;
        }

        // SAFETY: CreateEventW with null security and name is valid.
        let stop_event = unsafe { win32::CreateEventW(core::ptr::null(), 1, 0, core::ptr::null()) };
        if stop_event.is_null() {
            *self.inner.open_status.lock().expect("open_status poisoned") =
                appkernel::AK_STATUS_UNSUCCESSFUL;
            *self
                .inner
                .open_win32_error
                .lock()
                .expect("open_win32_error poisoned") = unsafe { win32::GetLastError() };
            let win32_error = *self
                .inner
                .open_win32_error
                .lock()
                .expect("open_win32_error poisoned");
            self.append_log(format!(
                "[backend] create stop event failed, win32={}",
                win32_error
            ));
            return false;
        }
        *self.inner.stop_event.lock().expect("stop_event poisoned") = stop_event as usize;

        let open_params = config::build_ak_open_params(
            &session_config,
            Some(appkernel_log_callback),
            Arc::as_ptr(&self.inner) as *mut c_void,
        );
        let mut session = core::ptr::null_mut();
        // SAFETY: AppKernel open with valid params.
        let status = unsafe { appkernel::AkOpen(&open_params, &mut session) };
        if status != appkernel::AK_STATUS_SUCCESS {
            *self.inner.open_status.lock().expect("open_status poisoned") = status;
            *self
                .inner
                .open_win32_error
                .lock()
                .expect("open_win32_error poisoned") = unsafe { win32::GetLastError() };
            let win32_error = *self
                .inner
                .open_win32_error
                .lock()
                .expect("open_win32_error poisoned");
            self.append_log(format!(
                "[backend] open session failed, status={}, win32={}",
                format_status_hex(status),
                win32_error
            ));
            // SAFETY: cleanup event handle.
            unsafe { win32::CloseHandle(stop_event) };
            *self.inner.stop_event.lock().expect("stop_event poisoned") = 0;
            return false;
        }

        *session_guard = session as usize;
        *self
            .inner
            .open_succeeded
            .lock()
            .expect("open_succeeded poisoned") = true;

        let mut session_state = appkernel::AkSessionState {
            lifecycle: appkernel::AkLifecycleState::Init,
            session_id: 0,
            heartbeat_running: 0,
            transport_ready: 0,
            disk_count: 0,
            appkernel_version_be: 0,
            kmdf_version_be: 0,
            scsi_version_be: 0,
            last_error: 0,
        };
        // SAFETY: valid session query.
        let query_status = unsafe { appkernel::AkQuerySessionState(session, &mut session_state) };
        if query_status == appkernel::AK_STATUS_SUCCESS {
            self.append_log(format!(
                "[backend] session opened, id={}, lifecycle={}, transport={}",
                session_state.session_id,
                lifecycle_to_text(session_state.lifecycle),
                if session_state.transport_ready != 0 {
                    "ready"
                } else {
                    "not-ready"
                }
            ));
            self.append_log(format!(
                "[backend] appkernel={}, kmdf={}, scsi={}",
                format_version_be(session_state.appkernel_version_be),
                format_version_be(session_state.kmdf_version_be),
                format_version_be(session_state.scsi_version_be)
            ));
        } else {
            self.append_log("[backend] session opened".to_string());
        }
        self.append_log(format!(
            "[backend] sessionConfig heartbeatIntervalMs={}, initialEventQueueCapacity={}",
            session_config.heartbeat_interval_ms, session_config.initial_event_queue_capacity
        ));

        let event_context = Arc::new(Self {
            inner: Arc::clone(&self.inner),
        });
        let thread = std::thread::spawn(move || event_context.run_event_loop());
        *self
            .inner
            .event_thread
            .lock()
            .expect("event_thread poisoned") = Some(thread);
        true
    }

    pub fn close(&self) {
        let _ = self.remove_all_managed_disks(true);
        self.inner.stop.store(true, Ordering::Relaxed);

        let stop_event = *self.inner.stop_event.lock().expect("stop_event poisoned");
        if stop_event != 0 {
            // SAFETY: event handle created by CreateEventW.
            unsafe { win32::SetEvent(stop_event as win32::Handle) };
        }

        if let Some(thread) = self
            .inner
            .event_thread
            .lock()
            .expect("event_thread poisoned")
            .take()
        {
            let _ = thread.join();
        }

        let mut session_guard = self.inner.session.lock().expect("session poisoned");
        if *session_guard != 0 {
            self.append_log("[backend] closing session".to_string());
            // SAFETY: valid session while open.
            unsafe { appkernel::AkClose(*session_guard as *mut appkernel::AkSession) };
            *session_guard = 0;
        }

        self.discard_all_disk_runtime_state();

        let mut stop_event_guard = self.inner.stop_event.lock().expect("stop_event poisoned");
        if *stop_event_guard != 0 {
            // SAFETY: valid handle.
            unsafe { win32::CloseHandle(*stop_event_guard as win32::Handle) };
            *stop_event_guard = 0;
        }
    }

    pub fn append_log(&self, text: String) {
        append_log_inner(&self.inner, text);
    }

    pub fn query_session_state_text(&self) -> String {
        let session = *self.inner.session.lock().expect("session poisoned");
        if session == 0 {
            if !*self
                .inner
                .open_succeeded
                .lock()
                .expect("open_succeeded poisoned")
            {
                let status = *self.inner.open_status.lock().expect("open_status poisoned");
                return format!("open-failed({})", format_status_hex(status));
            }
            return String::from("closed");
        }

        let mut session_state = appkernel::AkSessionState {
            lifecycle: appkernel::AkLifecycleState::Init,
            session_id: 0,
            heartbeat_running: 0,
            transport_ready: 0,
            disk_count: 0,
            appkernel_version_be: 0,
            kmdf_version_be: 0,
            scsi_version_be: 0,
            last_error: 0,
        };
        // SAFETY: valid session query.
        let status = unsafe {
            appkernel::AkQuerySessionState(session as *mut appkernel::AkSession, &mut session_state)
        };
        if status != appkernel::AK_STATUS_SUCCESS {
            return format!("query-failed({})", format_status_hex(status));
        }

        format!(
            "session={}, lifecycle={}, transport={}, disks={}",
            session_state.session_id,
            lifecycle_to_text(session_state.lifecycle),
            if session_state.transport_ready != 0 {
                "ready"
            } else {
                "not-ready"
            },
            session_state.disk_count
        )
    }

    pub fn snapshot_log_lines(&self) -> Vec<String> {
        self.inner
            .log_lines
            .lock()
            .expect("log_lines poisoned")
            .clone()
    }

    pub fn snapshot_managed_disks(&self) -> Vec<types::ManagedDiskSnapshot> {
        self.snapshot_disk_runtimes()
            .into_iter()
            .map(|runtime| self.make_managed_disk_snapshot(&runtime))
            .collect()
    }

    pub fn query_backend_stats(
        &self,
        out_stats: &mut types::BackendStatsSnapshot,
        out_error_text: Option<&mut String>,
    ) -> bool {
        let session = *self.inner.session.lock().expect("session poisoned");
        if session == 0 {
            if let Some(out_error_text) = out_error_text {
                *out_error_text = String::from("session-not-open");
            }
            return false;
        }

        let mut session_stats = appkernel::AkSessionStats::default();
        // SAFETY: valid session query.
        let status = unsafe {
            appkernel::AkQuerySessionStats(session as *mut appkernel::AkSession, &mut session_stats)
        };
        if status != appkernel::AK_STATUS_SUCCESS {
            if let Some(out_error_text) = out_error_text {
                *out_error_text = format_status_hex(status);
            }
            return false;
        }

        out_stats.heartbeat_sent = session_stats.heartbeat_sent;
        out_stats.command_failures = session_stats.command_failures;
        out_stats.protocol_failures = session_stats.protocol_failures;
        out_stats.events_queued = session_stats.events_queued;
        out_stats.events_dropped = session_stats.events_dropped;
        out_stats.disk_count = self.snapshot_disk_runtimes().len() as u64;
        true
    }

    pub fn query_debug_snapshot(
        &self,
        out_snapshot: &mut types::DebugSnapshot,
        out_error_text: Option<&mut String>,
    ) -> bool {
        out_snapshot.session_state_text = self.query_session_state_text();
        out_snapshot.disks = self.snapshot_managed_disks();
        self.query_backend_stats(&mut out_snapshot.stats, out_error_text)
    }

    pub fn find_first_free_target(&self) -> u32 {
        let disk_runtimes = self.inner.disk_runtimes.lock().expect("disk map poisoned");
        for target_id in types::YUMEDISK_MIN_TARGET_ID..=types::YUMEDISK_MAX_USABLE_TARGET_ID {
            if !disk_runtimes.contains_key(&target_id) {
                return target_id;
            }
        }
        types::YUMEDISK_MAX_TARGETS
    }

    pub fn create_managed_disk(
        &self,
        mut disk_config: types::DiskConfig,
        media: Box<dyn Media>,
        out_error_text: Option<&mut String>,
    ) -> bool {
        let session = *self.inner.session.lock().expect("session poisoned");
        if session == 0 {
            if let Some(out_error_text) = out_error_text {
                *out_error_text = String::from("session-not-open");
            }
            return false;
        }

        if disk_config.target_id == types::YUMEDISK_MAX_TARGETS {
            disk_config.target_id = self.find_first_free_target();
            if disk_config.target_id >= types::YUMEDISK_MAX_TARGETS {
                if let Some(out_error_text) = out_error_text {
                    *out_error_text = String::from("no-free-target");
                }
                return false;
            }
        }

        if let Err(error) = config::validate_create_disk_inputs(&disk_config, Some(media.as_ref()))
        {
            if let Some(out_error_text) = out_error_text {
                *out_error_text = String::from(error.as_code());
            }
            return false;
        }

        if self.disk_runtime_exists(disk_config.target_id) {
            if let Some(out_error_text) = out_error_text {
                *out_error_text = String::from("target-already-exists");
            }
            return false;
        }

        let visible_disks_before_create = scan::enumerate_visible_yumedisks();
        let runtime = Arc::new(RwLock::new(DiskRuntime::new(&disk_config)));
        {
            let mut runtime_guard = runtime.write().expect("disk runtime poisoned");
            runtime_guard.media.instance = Some(media);
        }

        self.insert_disk_runtime(Arc::clone(&runtime));

        let params = config::build_ak_disk_params(&disk_config);
        let mut handle = core::ptr::null_mut();
        // SAFETY: valid session, media ops and runtime context.
        let status = unsafe {
            appkernel::AkCreateDisk(
                session as *mut appkernel::AkSession,
                &params,
                &AK_MEDIA_OPS,
                Arc::as_ptr(&runtime) as *mut c_void,
                &mut handle,
            )
        };
        if status != appkernel::AK_STATUS_SUCCESS {
            self.erase_disk_runtime(disk_config.target_id);
            {
                let mut runtime_guard = runtime.write().expect("disk runtime poisoned");
                runtime_guard.staging.clear_locked();
                runtime_guard.media.instance = None;
            }
            if let Some(out_error_text) = out_error_text {
                *out_error_text = format_status_hex(status);
            }
            self.append_log(format!(
                "[backend] create failed, target={}, status={}",
                disk_config.target_id,
                format_status_hex(status)
            ));
            return false;
        }

        {
            let mut runtime_guard = runtime.write().expect("disk runtime poisoned");
            runtime_guard.lifecycle.handle = handle as usize;
        }

        let runtime_guard = runtime.read().expect("disk runtime poisoned");
        self.append_log(format!(
            "[backend] created target={}, diskBytes={}, readOnly={}, queueDepth={}, writeSlotBytes={}, readWorkerCount={}, writeWorkerCount={}, ackBatchMaxRanges={}",
            disk_config.target_id,
            runtime_guard.metadata.disk_size_bytes,
            read_only_to_text(runtime_guard.metadata.read_only),
            runtime_guard.queue_config.queue_depth,
            runtime_guard.queue_config.write_slot_bytes,
            runtime_guard.queue_config.read_worker_count,
            runtime_guard.queue_config.write_worker_count,
            runtime_guard.queue_config.ack_batch_max_ranges
        ));
        drop(runtime_guard);

        if self.try_refresh_disk_runtime_identity(
            &runtime,
            Some(&visible_disks_before_create),
            types::DISK_ARRIVAL_TIMEOUT_MS,
        ) {
            let runtime_guard = runtime.read().expect("disk runtime poisoned");
            self.append_log(format!(
                "[backend] visiblePath={}, physicalDrive={}",
                runtime_guard.metadata.identity.path,
                scan::make_physical_drive_path(runtime_guard.metadata.identity.device_number)
            ));
        } else {
            self.append_log(format!(
                "[backend] visiblePath=<pending-enumeration>, target={}",
                disk_config.target_id
            ));
        }

        true
    }

    pub fn remove_managed_disk(&self, target_id: u32, out_error_text: Option<&mut String>) -> bool {
        let session = *self.inner.session.lock().expect("session poisoned");
        if session == 0 {
            if let Some(out_error_text) = out_error_text {
                *out_error_text = String::from("session-not-open");
            }
            return false;
        }

        let Some(runtime) = self.find_disk_runtime(target_id) else {
            if let Some(out_error_text) = out_error_text {
                *out_error_text = String::from("target-not-found");
            }
            return false;
        };

        let handle = runtime
            .read()
            .expect("disk runtime poisoned")
            .lifecycle
            .handle;
        if handle == 0 {
            {
                let mut runtime_guard = runtime.write().expect("disk runtime poisoned");
                runtime_guard.staging.clear_locked();
                runtime_guard.media.instance = None;
            }
            self.erase_disk_runtime(target_id);
            self.append_log(format!("[backend] removed target={}", target_id));
            return true;
        }

        // SAFETY: valid disk handle.
        let status = unsafe { appkernel::AkRemoveDisk(handle as *mut appkernel::AkDisk) };
        if status != appkernel::AK_STATUS_SUCCESS {
            if let Some(out_error_text) = out_error_text {
                *out_error_text = format_status_hex(status);
            }
            self.append_log(format!(
                "[backend] remove failed, target={}, status={}",
                target_id,
                format_status_hex(status)
            ));
            return false;
        }

        {
            let mut runtime_guard = runtime.write().expect("disk runtime poisoned");
            runtime_guard.lifecycle.handle = 0;
            runtime_guard.staging.clear_locked();
            runtime_guard.media.instance = None;
        }
        self.erase_disk_runtime(target_id);
        self.append_log(format!("[backend] removed target={}", target_id));
        true
    }

    pub fn remove_all_managed_disks(&self, closing: bool) -> bool {
        let runtime_list = self.snapshot_disk_runtimes();
        let session = *self.inner.session.lock().expect("session poisoned");
        let mut removed_target_ids = Vec::new();
        let mut ok = true;

        for runtime in runtime_list {
            let target_id = runtime
                .read()
                .expect("disk runtime poisoned")
                .metadata
                .target_id;
            let handle = runtime
                .read()
                .expect("disk runtime poisoned")
                .lifecycle
                .handle;

            if handle != 0 && session != 0 {
                // SAFETY: valid disk handle.
                let status = unsafe { appkernel::AkRemoveDisk(handle as *mut appkernel::AkDisk) };
                if status != appkernel::AK_STATUS_SUCCESS {
                    self.append_log(format!("[backend] remove all failed, target={}", target_id));
                    ok = false;
                    if !closing {
                        continue;
                    }
                } else {
                    runtime
                        .write()
                        .expect("disk runtime poisoned")
                        .lifecycle
                        .handle = 0;
                }
            }

            {
                let mut runtime_guard = runtime.write().expect("disk runtime poisoned");
                runtime_guard.staging.clear_locked();
                runtime_guard.media.instance = None;
            }
            removed_target_ids.push(target_id);
        }

        let mut disk_runtimes = self.inner.disk_runtimes.lock().expect("disk map poisoned");
        for target_id in removed_target_ids {
            disk_runtimes.remove(&target_id);
        }

        self.append_log("[backend] removedAll=true".to_string());
        ok || closing
    }
}

unsafe extern "C" fn appkernel_log_callback(log_ctx: *mut c_void, level: i32, text: *const i8) {
    if log_ctx.is_null() {
        return;
    }
    // SAFETY: log_ctx is passed from BackendContext::open as Arc<Inner> pointee address and remains valid until close.
    let inner = unsafe { &*(log_ctx as *const Inner) };
    let wide_text = wide_from_text(text);
    append_log_inner(inner, format!("[AppKernel][{}] {}", level, wide_text));
}

unsafe extern "C" fn host_read_bytes(
    media_ctx: *mut c_void,
    op: *const appkernel::AkReadOp,
    out_buffer: *mut c_void,
    out_data_length: *mut u32,
) -> appkernel::AkStatus {
    if media_ctx.is_null() || op.is_null() || out_buffer.is_null() || out_data_length.is_null() {
        return appkernel::AK_STATUS_INVALID_PARAMETER;
    }

    // SAFETY: pointers validated above and owned by AppKernel callback contract.
    let runtime = unsafe { &*(media_ctx as *const RwLock<DiskRuntime>) };
    let op = unsafe { &*op };
    if op.data_length == 0 {
        unsafe { *out_data_length = 0 };
        return appkernel::AK_STATUS_SUCCESS;
    }

    let request_begin = op.offset_bytes;
    let request_end = request_begin.saturating_add(u64::from(op.data_length));
    let runtime_guard = runtime.read().expect("disk runtime poisoned");
    if request_end < request_begin || request_end > runtime_guard.metadata.disk_size_bytes {
        unsafe { *out_data_length = 0 };
        return appkernel::AK_STATUS_INVALID_PARAMETER;
    }

    let buffer =
        unsafe { std::slice::from_raw_parts_mut(out_buffer as *mut u8, op.data_length as usize) };
    let Some(media) = runtime_guard.media.instance.as_ref() else {
        unsafe { *out_data_length = 0 };
        return appkernel::AK_STATUS_UNSUCCESSFUL;
    };

    if media.read_locked(request_begin, buffer).is_err() {
        unsafe { *out_data_length = 0 };
        return appkernel::AK_STATUS_UNSUCCESSFUL;
    }

    runtime_guard
        .staging
        .overlay_read_locked(request_begin, buffer);
    unsafe { *out_data_length = op.data_length };
    appkernel::AK_STATUS_SUCCESS
}

unsafe extern "C" fn host_stage_write(
    media_ctx: *mut c_void,
    op: *const appkernel::AkWriteOp,
    data_buffer: *const c_void,
    data_length: u32,
) -> appkernel::AkStatus {
    if media_ctx.is_null() || op.is_null() {
        return appkernel::AK_STATUS_INVALID_PARAMETER;
    }

    // SAFETY: pointers validated above and owned by AppKernel callback contract.
    let runtime = unsafe { &*(media_ctx as *const RwLock<DiskRuntime>) };
    let op = unsafe { &*op };
    let data = if data_length == 0 || data_buffer.is_null() {
        &[]
    } else {
        unsafe { std::slice::from_raw_parts(data_buffer as *const u8, data_length as usize) }
    };
    let mut runtime_guard = runtime.write().expect("disk runtime poisoned");
    let disk_size_bytes = runtime_guard.metadata.disk_size_bytes;
    runtime_guard
        .staging
        .stage_write_locked(op, data, disk_size_bytes)
}

const AK_MEDIA_OPS: appkernel::AkMediaOps = appkernel::AkMediaOps {
    read_bytes: Some(host_read_bytes),
    stage_write: Some(host_stage_write),
};

fn commit_disk_runtime_staging(runtime: &Arc<RwLock<DiskRuntime>>, event_id: u64) -> bool {
    let mut runtime_guard = runtime.write().expect("disk runtime poisoned");
    let disk_size_bytes = runtime_guard.metadata.disk_size_bytes;
    let Some(media) = runtime_guard.media.instance.take() else {
        return false;
    };
    let result = runtime_guard
        .staging
        .commit_locked(event_id, disk_size_bytes, |offset, data| {
            media.write_locked(offset, data)
        });
    runtime_guard.media.instance = Some(media);
    result
}

fn reject_disk_runtime_staging(runtime: &Arc<RwLock<DiskRuntime>>, event_id: u64) {
    runtime
        .write()
        .expect("disk runtime poisoned")
        .staging
        .reject_locked(event_id);
}

#[cfg(test)]
mod tests {
    use super::BackendContext;
    use crate::error::BackendError;
    use crate::media::Media;
    use crate::types::DiskConfig;

    struct DummyMedia {
        size_bytes: u64,
    }

    impl Media for DummyMedia {
        fn size_bytes(&self) -> u64 {
            self.size_bytes
        }

        fn read_locked(&self, _offset: u64, buffer: &mut [u8]) -> Result<(), BackendError> {
            buffer.fill(0);
            Ok(())
        }

        fn write_locked(&self, _offset: u64, _data: &[u8]) -> Result<(), BackendError> {
            Ok(())
        }
    }

    #[test]
    fn find_first_free_target_defaults_to_zero() {
        let context = BackendContext::new();
        assert_eq!(context.find_first_free_target(), 0);
    }

    #[test]
    fn session_config_roundtrip() {
        let context = BackendContext::new();
        let config = context.session_config();
        assert_eq!(config.heartbeat_interval_ms, 1000);
        assert_eq!(config.initial_event_queue_capacity, 1024);
    }

    #[test]
    fn create_disk_requires_open_session() {
        let context = BackendContext::new();
        let disk_config = DiskConfig {
            disk_size_bytes: 4096,
            ..DiskConfig::default()
        };
        let mut error = String::new();
        let ok = context.create_managed_disk(
            disk_config,
            Box::new(DummyMedia { size_bytes: 4096 }),
            Some(&mut error),
        );
        assert!(!ok);
        assert_eq!(error, "session-not-open");
    }
}
