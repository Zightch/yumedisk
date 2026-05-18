#[cfg(target_os = "windows")]
mod imp {
    use std::ffi::OsStr;
    use std::iter;
    use std::os::windows::ffi::OsStrExt;
    use std::ptr;
    use std::thread;

    use tauri::AppHandle;
    use windows_sys::Win32::Foundation::{CloseHandle, ERROR_ALREADY_EXISTS, HANDLE};
    use windows_sys::Win32::Security::SECURITY_ATTRIBUTES;
    use windows_sys::Win32::System::Threading::{
        CreateEventW, CreateMutexW, SetEvent, WaitForSingleObject, INFINITE,
    };

    const SINGLE_INSTANCE_MUTEX_NAME: &str = "Local\\YumeDisk.TauriClient.Singleton";
    const SINGLE_INSTANCE_WAKE_EVENT_NAME: &str = "Local\\YumeDisk.TauriClient.WakeMainWindow";

    type RawHandle = usize;

    pub struct SingleInstanceGuard {
        mutex_handle: RawHandle,
        wake_event_handle: RawHandle,
    }

    impl Drop for SingleInstanceGuard {
        fn drop(&mut self) {
            close_raw_handle(self.wake_event_handle);
            close_raw_handle(self.mutex_handle);
        }
    }

    pub enum StartupAction {
        Continue(SingleInstanceGuard),
        Exit,
    }

    pub fn initialize() -> StartupAction {
        let mutex_handle = create_mutex(SINGLE_INSTANCE_MUTEX_NAME);
        if mutex_handle.is_null() {
            return StartupAction::Continue(SingleInstanceGuard {
                mutex_handle: raw_handle(mutex_handle),
                wake_event_handle: 0,
            });
        }

        let last_error = std::io::Error::last_os_error()
            .raw_os_error()
            .unwrap_or_default() as u32;
        if last_error == ERROR_ALREADY_EXISTS {
            signal_existing_instance();
            close_handle(mutex_handle);
            return StartupAction::Exit;
        }

        let wake_event_handle = create_event(SINGLE_INSTANCE_WAKE_EVENT_NAME);
        StartupAction::Continue(SingleInstanceGuard {
            mutex_handle: raw_handle(mutex_handle),
            wake_event_handle: raw_handle(wake_event_handle),
        })
    }

    pub fn spawn_wake_listener(app: &AppHandle, guard: &SingleInstanceGuard, wake: fn(&AppHandle)) {
        if guard.wake_event_handle == 0 {
            return;
        }

        let app = app.clone();
        let wake_event_handle = guard.wake_event_handle;

        thread::spawn(move || loop {
            let wait_result =
                unsafe { WaitForSingleObject(handle_from_raw(wake_event_handle), INFINITE) };
            if wait_result != 0 {
                break;
            }

            let app_handle = app.clone();
            let _ = app.run_on_main_thread(move || {
                wake(&app_handle);
            });
        });
    }

    fn signal_existing_instance() {
        let wake_event_handle = create_event(SINGLE_INSTANCE_WAKE_EVENT_NAME);
        if !wake_event_handle.is_null() {
            unsafe {
                SetEvent(wake_event_handle);
            }
            close_handle(wake_event_handle);
        }
    }

    fn create_mutex(name: &str) -> HANDLE {
        let name = to_wide(name);
        unsafe { CreateMutexW(ptr::null::<SECURITY_ATTRIBUTES>(), 0, name.as_ptr()) }
    }

    fn create_event(name: &str) -> HANDLE {
        let name = to_wide(name);
        unsafe { CreateEventW(ptr::null::<SECURITY_ATTRIBUTES>(), 0, 0, name.as_ptr()) }
    }

    fn close_handle(handle: HANDLE) {
        if !handle.is_null() {
            unsafe {
                CloseHandle(handle);
            }
        }
    }

    fn close_raw_handle(handle: RawHandle) {
        if handle != 0 {
            close_handle(handle_from_raw(handle));
        }
    }

    fn raw_handle(handle: HANDLE) -> RawHandle {
        handle as RawHandle
    }

    fn handle_from_raw(handle: RawHandle) -> HANDLE {
        handle as HANDLE
    }

    fn to_wide(value: &str) -> Vec<u16> {
        OsStr::new(value)
            .encode_wide()
            .chain(iter::once(0))
            .collect()
    }
}

#[cfg(not(target_os = "windows"))]
mod imp {
    use tauri::AppHandle;

    pub struct SingleInstanceGuard;

    pub enum StartupAction {
        Continue(SingleInstanceGuard),
        Exit,
    }

    pub fn initialize() -> StartupAction {
        StartupAction::Continue(SingleInstanceGuard)
    }

    pub fn spawn_wake_listener(_: &AppHandle, _: &SingleInstanceGuard, _: fn(&AppHandle)) {}
}

pub use imp::{initialize, spawn_wake_listener, SingleInstanceGuard, StartupAction};
