use core::ffi::c_void;

pub type Handle = *mut c_void;
pub type Bool = i32;

pub const CP_UTF8: u32 = 65001;
pub const CP_ACP: u32 = 0;

pub const ERROR_SUCCESS: u32 = 0;
pub const ERROR_INVALID_PARAMETER: u32 = 87;

#[link(name = "kernel32")]
unsafe extern "system" {
    pub fn MultiByteToWideChar(
        code_page: u32,
        flags: u32,
        multi_byte_str: *const i8,
        cb_multi_byte: i32,
        wide_char_str: *mut u16,
        cch_wide_char: i32,
    ) -> i32;

    pub fn CloseHandle(handle: Handle) -> Bool;
    pub fn OutputDebugStringW(output_string: *const u16);
    pub fn CreateEventW(
        event_attributes: *const c_void,
        manual_reset: Bool,
        initial_state: Bool,
        name: *const u16,
    ) -> Handle;
    pub fn SetEvent(handle: Handle) -> Bool;
    pub fn GetLastError() -> u32;
    pub fn Sleep(milliseconds: u32);
}
