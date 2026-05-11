use std::ffi::c_void;
type Handle = *mut c_void;
type Bool = i32;

const TRUE_BOOL: Bool = 1;
const GENERIC_READ: u32 = 0x8000_0000;
const GENERIC_WRITE: u32 = 0x4000_0000;
const FILE_SHARE_READ: u32 = 0x0000_0001;
const FILE_SHARE_WRITE: u32 = 0x0000_0002;
const OPEN_EXISTING: u32 = 3;
const FILE_ATTRIBUTE_NORMAL: u32 = 0x0000_0080;

#[link(name = "kernel32")]
unsafe extern "system" {
    fn CreateFileW(
        file_name: *const u16,
        desired_access: u32,
        share_mode: u32,
        security_attributes: *const c_void,
        creation_disposition: u32,
        flags_and_attributes: u32,
        template_file: Handle,
    ) -> Handle;

    fn ReadFile(
        file: Handle,
        buffer: *mut c_void,
        bytes_to_read: u32,
        bytes_read: *mut u32,
        overlapped: *mut c_void,
    ) -> Bool;

    fn WriteFile(
        file: Handle,
        buffer: *const c_void,
        bytes_to_write: u32,
        bytes_written: *mut u32,
        overlapped: *mut c_void,
    ) -> Bool;

    fn SetFilePointerEx(
        file: Handle,
        distance_to_move: i64,
        new_file_pointer: *mut i64,
        move_method: u32,
    ) -> Bool;

    fn CloseHandle(handle: Handle) -> Bool;
    fn GetLastError() -> u32;
}

const FILE_BEGIN: u32 = 0;
const INVALID_HANDLE_VALUE: Handle = -1isize as Handle;

pub struct DeviceHandle {
    handle: Handle,
}

impl DeviceHandle {
    pub fn open(path: &str, read_write: bool) -> Result<Self, String> {
        let mut wide_path: Vec<u16> = path.encode_utf16().collect();
        wide_path.push(0);
        let desired_access = if read_write {
            GENERIC_READ | GENERIC_WRITE
        } else {
            GENERIC_READ
        };
        // SAFETY: Windows API call with a null-terminated UTF-16 path.
        let handle = unsafe {
            CreateFileW(
                wide_path.as_ptr(),
                desired_access,
                FILE_SHARE_READ | FILE_SHARE_WRITE,
                core::ptr::null(),
                OPEN_EXISTING,
                FILE_ATTRIBUTE_NORMAL,
                core::ptr::null_mut(),
            )
        };
        if handle == INVALID_HANDLE_VALUE {
            // SAFETY: GetLastError is thread-local and valid after CreateFileW failure.
            let error = unsafe { GetLastError() };
            return Err(format!("open-device-failed path={} win32={}", path, error));
        }
        Ok(Self { handle })
    }

    pub fn read_exact_at(&self, offset_bytes: u64, buffer: &mut [u8]) -> Result<(), String> {
        self.seek_to(offset_bytes)?;
        let mut read_total = 0usize;
        while read_total < buffer.len() {
            let mut bytes_read = 0u32;
            // SAFETY: buffer is valid and large enough for the requested chunk.
            let ok = unsafe {
                ReadFile(
                    self.handle,
                    buffer[read_total..].as_mut_ptr() as *mut c_void,
                    (buffer.len() - read_total)
                        .min(u32::MAX as usize)
                        .try_into()
                        .map_err(|_| "read-size-overflow".to_string())?,
                    &mut bytes_read,
                    core::ptr::null_mut(),
                )
            };
            if ok != TRUE_BOOL {
                // SAFETY: GetLastError is thread-local and valid after ReadFile failure.
                let error = unsafe { GetLastError() };
                return Err(format!(
                    "read-failed offset={} win32={}",
                    offset_bytes, error
                ));
            }
            if bytes_read == 0 {
                return Err(format!("read-short offset={} bytes=0", offset_bytes));
            }
            read_total += bytes_read as usize;
        }
        Ok(())
    }

    pub fn write_all_at(&self, offset_bytes: u64, data: &[u8]) -> Result<(), String> {
        self.seek_to(offset_bytes)?;
        let mut written_total = 0usize;
        while written_total < data.len() {
            let mut bytes_written = 0u32;
            // SAFETY: data slice is valid for the requested chunk.
            let ok = unsafe {
                WriteFile(
                    self.handle,
                    data[written_total..].as_ptr() as *const c_void,
                    (data.len() - written_total)
                        .min(u32::MAX as usize)
                        .try_into()
                        .map_err(|_| "write-size-overflow".to_string())?,
                    &mut bytes_written,
                    core::ptr::null_mut(),
                )
            };
            if ok != TRUE_BOOL {
                // SAFETY: GetLastError is thread-local and valid after WriteFile failure.
                let error = unsafe { GetLastError() };
                return Err(format!(
                    "write-failed offset={} win32={}",
                    offset_bytes, error
                ));
            }
            if bytes_written == 0 {
                return Err(format!("write-short offset={} bytes=0", offset_bytes));
            }
            written_total += bytes_written as usize;
        }
        Ok(())
    }

    fn seek_to(&self, offset_bytes: u64) -> Result<(), String> {
        let mut new_position = 0i64;
        // SAFETY: the handle is valid and the offset is passed by value.
        let ok = unsafe {
            SetFilePointerEx(
                self.handle,
                offset_bytes as i64,
                &mut new_position,
                FILE_BEGIN,
            )
        };
        if ok != TRUE_BOOL {
            // SAFETY: GetLastError is thread-local and valid after SetFilePointerEx failure.
            let error = unsafe { GetLastError() };
            return Err(format!(
                "seek-failed offset={} win32={}",
                offset_bytes, error
            ));
        }
        Ok(())
    }
}

impl Drop for DeviceHandle {
    fn drop(&mut self) {
        if self.handle != INVALID_HANDLE_VALUE {
            // SAFETY: handle came from CreateFileW and is owned here.
            unsafe {
                CloseHandle(self.handle);
            }
            self.handle = INVALID_HANDLE_VALUE;
        }
    }
}
