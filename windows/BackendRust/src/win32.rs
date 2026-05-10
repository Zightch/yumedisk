use core::ffi::c_void;

pub type Handle = *mut c_void;
pub type Hdevinfo = Handle;
pub type Dword = u32;
pub type Word = u16;
pub type Bool = i32;
pub type Ulong = u32;
pub type Wchar = u16;

pub const INVALID_HANDLE_VALUE: Handle = -1isize as Handle;
pub const TRUE_BOOL: Bool = 1;
pub const FALSE_BOOL: Bool = 0;

pub const CP_UTF8: u32 = 65001;
pub const CP_ACP: u32 = 0;

pub const ERROR_SUCCESS: u32 = 0;
pub const ERROR_INVALID_PARAMETER: u32 = 87;
pub const ERROR_NOT_ENOUGH_MEMORY: u32 = 8;
pub const ERROR_NO_MORE_ITEMS: u32 = 259;

pub const GENERIC_READ: u32 = 0x8000_0000;
pub const FILE_SHARE_READ: u32 = 0x0000_0001;
pub const FILE_SHARE_WRITE: u32 = 0x0000_0002;
pub const OPEN_EXISTING: u32 = 3;
pub const FILE_ATTRIBUTE_NORMAL: u32 = 0x0000_0080;

pub const DIGCF_PRESENT: u32 = 0x0000_0002;
pub const DIGCF_DEVICEINTERFACE: u32 = 0x0000_0010;

pub const FILE_DEVICE_MASS_STORAGE: u32 = 0x0000_002d;
pub const FILE_DEVICE_DISK: u32 = 0x0000_0007;
pub const METHOD_BUFFERED: u32 = 0;
pub const FILE_ANY_ACCESS: u32 = 0;

pub const fn ctl_code(device_type: u32, function: u32, method: u32, access: u32) -> u32 {
    (device_type << 16) | (access << 14) | (function << 2) | method
}

pub const IOCTL_STORAGE_QUERY_PROPERTY: u32 = ctl_code(
    FILE_DEVICE_MASS_STORAGE,
    0x0500,
    METHOD_BUFFERED,
    FILE_ANY_ACCESS,
);
pub const IOCTL_STORAGE_GET_DEVICE_NUMBER: u32 = ctl_code(
    FILE_DEVICE_MASS_STORAGE,
    0x0420,
    METHOD_BUFFERED,
    FILE_ANY_ACCESS,
);
pub const IOCTL_DISK_GET_LENGTH_INFO: u32 =
    ctl_code(FILE_DEVICE_DISK, 0x0017, METHOD_BUFFERED, FILE_ANY_ACCESS);

#[repr(C)]
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Guid {
    pub data1: u32,
    pub data2: u16,
    pub data3: u16,
    pub data4: [u8; 8],
}

pub const GUID_DEVINTERFACE_DISK: Guid = Guid {
    data1: 0x53f56307,
    data2: 0xb6bf,
    data3: 0x11d0,
    data4: [0x94, 0xf2, 0x00, 0xa0, 0xc9, 0x1e, 0xfb, 0x8b],
};

#[repr(C)]
#[derive(Clone, Copy)]
pub struct SpDeviceInterfaceData {
    pub cb_size: u32,
    pub interface_class_guid: Guid,
    pub flags: u32,
    pub reserved: usize,
}

#[repr(C)]
#[derive(Clone, Copy)]
pub struct SpDeviceInterfaceDetailDataW {
    pub cb_size: u32,
    pub device_path: [u16; 1],
}

#[repr(C)]
#[derive(Clone, Copy)]
pub struct StoragePropertyQuery {
    pub property_id: u32,
    pub query_type: u32,
    pub additional_parameters: [u8; 1],
}

#[repr(C)]
#[derive(Clone, Copy, Default)]
pub struct StorageDescriptorHeader {
    pub version: u32,
    pub size: u32,
}

#[repr(C)]
#[derive(Clone, Copy)]
pub struct StorageDeviceDescriptor {
    pub version: u32,
    pub size: u32,
    pub device_type: u8,
    pub device_type_modifier: u8,
    pub removable_media: u8,
    pub command_queueing: u8,
    pub vendor_id_offset: u32,
    pub product_id_offset: u32,
    pub product_revision_offset: u32,
    pub serial_number_offset: u32,
    pub bus_type: u32,
    pub raw_properties_length: u32,
    pub raw_device_properties: [u8; 1],
}

#[repr(C)]
#[derive(Clone, Copy)]
pub struct GetLengthInformation {
    pub length: i64,
}

#[repr(C)]
#[derive(Clone, Copy, Default)]
pub struct StorageDeviceNumber {
    pub device_type: u32,
    pub device_number: u32,
    pub partition_number: u32,
}

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

    pub fn CreateFileW(
        file_name: *const u16,
        desired_access: u32,
        share_mode: u32,
        security_attributes: *const c_void,
        creation_disposition: u32,
        flags_and_attributes: u32,
        template_file: Handle,
    ) -> Handle;

    pub fn DeviceIoControl(
        device: Handle,
        io_control_code: u32,
        in_buffer: *const c_void,
        in_buffer_size: u32,
        out_buffer: *mut c_void,
        out_buffer_size: u32,
        bytes_returned: *mut u32,
        overlapped: *mut c_void,
    ) -> Bool;

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
    pub fn GetTickCount64() -> u64;
    pub fn Sleep(milliseconds: u32);
}

#[link(name = "setupapi")]
unsafe extern "system" {
    pub fn SetupDiGetClassDevsW(
        class_guid: *const Guid,
        enumerator: *const u16,
        hwnd_parent: Handle,
        flags: u32,
    ) -> Hdevinfo;

    pub fn SetupDiEnumDeviceInterfaces(
        device_info_set: Hdevinfo,
        device_info_data: *mut c_void,
        interface_class_guid: *const Guid,
        member_index: u32,
        device_interface_data: *mut SpDeviceInterfaceData,
    ) -> Bool;

    pub fn SetupDiGetDeviceInterfaceDetailW(
        device_info_set: Hdevinfo,
        device_interface_data: *mut SpDeviceInterfaceData,
        device_interface_detail_data: *mut SpDeviceInterfaceDetailDataW,
        device_interface_detail_data_size: u32,
        required_size: *mut u32,
        device_info_data: *mut c_void,
    ) -> Bool;

    pub fn SetupDiDestroyDeviceInfoList(device_info_set: Hdevinfo) -> Bool;
}
