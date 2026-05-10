use std::cmp::Ordering;

use crate::win32;

#[derive(Debug, Clone, Default, PartialEq, Eq)]
pub struct DiskIdentity {
    pub path: String,
    pub vendor: String,
    pub product: String,
    pub length_bytes: u64,
    pub device_number: u32,
}

fn utf16_from_ansi(text: *const i8) -> String {
    if text.is_null() {
        return String::new();
    }

    // SAFETY: Windows API call with null output to query length.
    let length =
        unsafe { win32::MultiByteToWideChar(win32::CP_ACP, 0, text, -1, core::ptr::null_mut(), 0) };
    if length <= 1 {
        return String::new();
    }

    let mut buffer = vec![0u16; length as usize];
    // SAFETY: buffer is allocated with requested size.
    let written = unsafe {
        win32::MultiByteToWideChar(win32::CP_ACP, 0, text, -1, buffer.as_mut_ptr(), length)
    };
    if written <= 1 {
        return String::new();
    }
    if let Some(&0) = buffer.last() {
        buffer.pop();
    }
    String::from_utf16_lossy(&buffer)
}

fn wide_ptr_to_string(ptr: *const u16) -> String {
    if ptr.is_null() {
        return String::new();
    }
    let mut len = 0usize;
    // SAFETY: pointer comes from Win32 null-terminated strings.
    unsafe {
        while *ptr.add(len) != 0 {
            len += 1;
        }
        String::from_utf16_lossy(std::slice::from_raw_parts(ptr, len))
    }
}

fn trim_descriptor(text: String) -> String {
    text.trim().to_string()
}

fn contains_insensitive(haystack: &str, needle: &str) -> bool {
    haystack.to_lowercase().contains(&needle.to_lowercase())
}

fn enumerate_device_interfaces(guid: &win32::Guid) -> Vec<String> {
    let mut paths = Vec::new();
    // SAFETY: Win32 API call.
    let info = unsafe {
        win32::SetupDiGetClassDevsW(
            guid,
            core::ptr::null(),
            core::ptr::null_mut(),
            win32::DIGCF_DEVICEINTERFACE | win32::DIGCF_PRESENT,
        )
    };
    if info == win32::INVALID_HANDLE_VALUE {
        return paths;
    }

    let mut index = 0u32;
    loop {
        let mut interface_data = win32::SpDeviceInterfaceData {
            cb_size: std::mem::size_of::<win32::SpDeviceInterfaceData>() as u32,
            interface_class_guid: *guid,
            flags: 0,
            reserved: 0,
        };

        // SAFETY: Win32 API call.
        let ok = unsafe {
            win32::SetupDiEnumDeviceInterfaces(
                info,
                core::ptr::null_mut(),
                guid,
                index,
                &mut interface_data,
            )
        };
        if ok == 0 {
            // SAFETY: Win32 API call.
            let last_error = unsafe { win32::GetLastError() };
            if last_error == win32::ERROR_NO_MORE_ITEMS {
                break;
            }
            index += 1;
            continue;
        }

        let mut required_size = 0u32;
        // SAFETY: probe for size.
        unsafe {
            win32::SetupDiGetDeviceInterfaceDetailW(
                info,
                &mut interface_data,
                core::ptr::null_mut(),
                0,
                &mut required_size,
                core::ptr::null_mut(),
            );
        }
        if required_size == 0 {
            index += 1;
            continue;
        }

        let mut detail_buffer = vec![0u8; required_size as usize];
        let detail = detail_buffer.as_mut_ptr() as *mut win32::SpDeviceInterfaceDetailDataW;
        // SAFETY: buffer large enough.
        unsafe {
            (*detail).cb_size = std::mem::size_of::<win32::SpDeviceInterfaceDetailDataW>() as u32;
        }

        // SAFETY: Win32 API call.
        let ok = unsafe {
            win32::SetupDiGetDeviceInterfaceDetailW(
                info,
                &mut interface_data,
                detail,
                required_size,
                core::ptr::null_mut(),
                core::ptr::null_mut(),
            )
        };
        if ok != 0 {
            // SAFETY: field starts with null-terminated path.
            let path = unsafe { wide_ptr_to_string((*detail).device_path.as_ptr()) };
            paths.push(path);
        }

        index += 1;
    }

    // SAFETY: cleanup Win32 handle list.
    unsafe {
        win32::SetupDiDestroyDeviceInfoList(info);
    }
    paths
}

fn query_disk_identity(path: &str) -> Option<DiskIdentity> {
    let wide_path: Vec<u16> = path.encode_utf16().chain(std::iter::once(0)).collect();
    // SAFETY: Win32 API call.
    let handle = unsafe {
        win32::CreateFileW(
            wide_path.as_ptr(),
            win32::GENERIC_READ,
            win32::FILE_SHARE_READ | win32::FILE_SHARE_WRITE,
            core::ptr::null(),
            win32::OPEN_EXISTING,
            win32::FILE_ATTRIBUTE_NORMAL,
            core::ptr::null_mut(),
        )
    };
    if handle == win32::INVALID_HANDLE_VALUE {
        return None;
    }

    let mut identity = DiskIdentity {
        path: path.to_string(),
        vendor: String::new(),
        product: String::new(),
        length_bytes: 0,
        device_number: u32::MAX,
    };

    let query = win32::StoragePropertyQuery {
        property_id: 0,
        query_type: 0,
        additional_parameters: [0],
    };
    let mut header = win32::StorageDescriptorHeader::default();
    let mut bytes_returned = 0u32;

    // SAFETY: Win32 API call.
    let ok = unsafe {
        win32::DeviceIoControl(
            handle,
            win32::IOCTL_STORAGE_QUERY_PROPERTY,
            &query as *const _ as *const _,
            std::mem::size_of::<win32::StoragePropertyQuery>() as u32,
            &mut header as *mut _ as *mut _,
            std::mem::size_of::<win32::StorageDescriptorHeader>() as u32,
            &mut bytes_returned,
            core::ptr::null_mut(),
        )
    };
    if ok != 0 && header.size >= std::mem::size_of::<win32::StorageDeviceDescriptor>() as u32 {
        let mut descriptor_buffer = vec![0u8; header.size as usize];
        // SAFETY: Win32 API call.
        let ok = unsafe {
            win32::DeviceIoControl(
                handle,
                win32::IOCTL_STORAGE_QUERY_PROPERTY,
                &query as *const _ as *const _,
                std::mem::size_of::<win32::StoragePropertyQuery>() as u32,
                descriptor_buffer.as_mut_ptr() as *mut _,
                descriptor_buffer.len() as u32,
                &mut bytes_returned,
                core::ptr::null_mut(),
            )
        };
        if ok != 0 {
            // SAFETY: descriptor_buffer contains STORAGE_DEVICE_DESCRIPTOR.
            let descriptor =
                unsafe { &*(descriptor_buffer.as_ptr() as *const win32::StorageDeviceDescriptor) };
            if descriptor.vendor_id_offset != 0
                && (descriptor.vendor_id_offset as usize) < descriptor_buffer.len()
            {
                let ptr = unsafe {
                    descriptor_buffer
                        .as_ptr()
                        .add(descriptor.vendor_id_offset as usize) as *const i8
                };
                identity.vendor = trim_descriptor(utf16_from_ansi(ptr));
            }
            if descriptor.product_id_offset != 0
                && (descriptor.product_id_offset as usize) < descriptor_buffer.len()
            {
                let ptr = unsafe {
                    descriptor_buffer
                        .as_ptr()
                        .add(descriptor.product_id_offset as usize) as *const i8
                };
                identity.product = trim_descriptor(utf16_from_ansi(ptr));
            }
        }
    }

    let mut length_info = win32::GetLengthInformation { length: 0 };
    // SAFETY: Win32 API call.
    let ok = unsafe {
        win32::DeviceIoControl(
            handle,
            win32::IOCTL_DISK_GET_LENGTH_INFO,
            core::ptr::null(),
            0,
            &mut length_info as *mut _ as *mut _,
            std::mem::size_of::<win32::GetLengthInformation>() as u32,
            &mut bytes_returned,
            core::ptr::null_mut(),
        )
    };
    if ok != 0 {
        identity.length_bytes = length_info.length as u64;
    }

    let mut device_number = win32::StorageDeviceNumber::default();
    // SAFETY: Win32 API call.
    let ok = unsafe {
        win32::DeviceIoControl(
            handle,
            win32::IOCTL_STORAGE_GET_DEVICE_NUMBER,
            core::ptr::null(),
            0,
            &mut device_number as *mut _ as *mut _,
            std::mem::size_of::<win32::StorageDeviceNumber>() as u32,
            &mut bytes_returned,
            core::ptr::null_mut(),
        )
    };
    if ok != 0 {
        identity.device_number = device_number.device_number;
    }

    // SAFETY: cleanup file handle.
    unsafe {
        win32::CloseHandle(handle);
    }
    Some(identity)
}

fn is_target_disk_candidate(identity: &DiskIdentity) -> bool {
    contains_insensitive(&identity.vendor, "Zightch")
        && contains_insensitive(&identity.product, "YumeDisk")
}

pub fn enumerate_visible_yumedisks() -> Vec<DiskIdentity> {
    let interfaces = enumerate_device_interfaces(&win32::GUID_DEVINTERFACE_DISK);
    let mut identities = Vec::new();
    for path in interfaces {
        if let Some(identity) = query_disk_identity(&path) {
            if is_target_disk_candidate(&identity) {
                identities.push(identity);
            }
        }
    }

    identities.sort_by(
        |left, right| match left.device_number.cmp(&right.device_number) {
            Ordering::Equal => left.path.cmp(&right.path),
            order => order,
        },
    );
    identities
}

pub fn make_physical_drive_path(device_number: u32) -> String {
    if device_number == u32::MAX {
        return String::new();
    }
    format!(r"\\.\PhysicalDrive{}", device_number)
}
