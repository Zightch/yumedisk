#include "scan.h"

#include <SetupAPI.h>
#include <WinIoCtl.h>

#include <algorithm>
#include <cwctype>
#include <vector>

namespace YumeDisk::Scan {

namespace {

std::wstring Utf16FromAnsi(
    const char* text)
{
    int length;
    std::wstring result;

    if ((text == nullptr) || (*text == '\0')) {
        return {};
    }

    length = MultiByteToWideChar(CP_ACP, 0, text, -1, nullptr, 0);
    if (length <= 1) {
        return {};
    }

    result.resize((size_t)length);
    (void)MultiByteToWideChar(CP_ACP, 0, text, -1, result.data(), length);
    if (!result.empty() && result.back() == L'\0') {
        result.pop_back();
    }

    return result;
}

std::wstring TrimDescriptor(
    const std::wstring& text)
{
    size_t begin;
    size_t end;

    begin = 0;
    end = text.size();
    while ((begin < end) && (iswspace(text[begin]) != 0)) {
        begin += 1;
    }
    while ((end > begin) && (iswspace(text[end - 1]) != 0)) {
        end -= 1;
    }

    return text.substr(begin, end - begin);
}

bool ContainsInsensitive(
    const std::wstring& haystack,
    const std::wstring& needle)
{
    size_t start;
    size_t index;

    if (needle.empty() || (haystack.size() < needle.size())) {
        return false;
    }

    for (start = 0; start + needle.size() <= haystack.size(); ++start) {
        bool match;

        match = true;
        for (index = 0; index < needle.size(); ++index) {
            if (towlower(haystack[start + index]) != towlower(needle[index])) {
                match = false;
                break;
            }
        }

        if (match) {
            return true;
        }
    }

    return false;
}

std::vector<std::wstring> EnumerateDeviceInterfaces(
    const GUID* guid)
{
    std::vector<std::wstring> paths;
    HDEVINFO info;
    DWORD index;

    info = SetupDiGetClassDevsW(guid, nullptr, nullptr, DIGCF_DEVICEINTERFACE | DIGCF_PRESENT);
    if (info == INVALID_HANDLE_VALUE) {
        return paths;
    }

    for (index = 0;; ++index) {
        SP_DEVICE_INTERFACE_DATA interface_data{};
        DWORD required_size;
        std::vector<unsigned char> detail_buffer;
        PSP_DEVICE_INTERFACE_DETAIL_DATA_W detail;

        interface_data.cbSize = sizeof(interface_data);
        if (!SetupDiEnumDeviceInterfaces(info, nullptr, guid, index, &interface_data)) {
            if (GetLastError() == ERROR_NO_MORE_ITEMS) {
                break;
            }
            continue;
        }

        required_size = 0;
        (void)SetupDiGetDeviceInterfaceDetailW(
            info,
            &interface_data,
            nullptr,
            0,
            &required_size,
            nullptr);
        if (required_size == 0) {
            continue;
        }

        detail_buffer.resize(required_size, 0);
        detail = reinterpret_cast<PSP_DEVICE_INTERFACE_DETAIL_DATA_W>(detail_buffer.data());
        detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
        if (!SetupDiGetDeviceInterfaceDetailW(
                info,
                &interface_data,
                detail,
                required_size,
                nullptr,
                nullptr)) {
            continue;
        }

        paths.emplace_back(detail->DevicePath);
    }

    SetupDiDestroyDeviceInfoList(info);
    return paths;
}

bool QueryDiskIdentity(
    const std::wstring& path,
    DiskIdentity* identity)
{
    HANDLE handle;
    STORAGE_PROPERTY_QUERY query{};
    STORAGE_DESCRIPTOR_HEADER header{};
    DWORD bytes_returned;

    if (identity == nullptr) {
        return false;
    }

    identity->Path = path;
    identity->Vendor.clear();
    identity->Product.clear();
    identity->LengthBytes = 0;
    identity->DeviceNumber = std::numeric_limits<DWORD>::max();

    handle = CreateFileW(
        path.c_str(),
        GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        return false;
    }

    query.PropertyId = StorageDeviceProperty;
    query.QueryType = PropertyStandardQuery;
    bytes_returned = 0;

    if (DeviceIoControl(
            handle,
            IOCTL_STORAGE_QUERY_PROPERTY,
            &query,
            sizeof(query),
            &header,
            sizeof(header),
            &bytes_returned,
            nullptr) &&
        (header.Size >= sizeof(STORAGE_DEVICE_DESCRIPTOR))) {
        std::vector<unsigned char> descriptor_buffer;
        const STORAGE_DEVICE_DESCRIPTOR* descriptor;

        descriptor_buffer.resize(header.Size, 0);
        if (DeviceIoControl(
                handle,
                IOCTL_STORAGE_QUERY_PROPERTY,
                &query,
                sizeof(query),
                descriptor_buffer.data(),
                (DWORD)descriptor_buffer.size(),
                &bytes_returned,
                nullptr)) {
            descriptor = reinterpret_cast<const STORAGE_DEVICE_DESCRIPTOR*>(descriptor_buffer.data());
            if ((descriptor->VendorIdOffset != 0) &&
                (descriptor->VendorIdOffset < descriptor_buffer.size())) {
                identity->Vendor = TrimDescriptor(Utf16FromAnsi(
                    reinterpret_cast<const char*>(descriptor_buffer.data() + descriptor->VendorIdOffset)));
            }
            if ((descriptor->ProductIdOffset != 0) &&
                (descriptor->ProductIdOffset < descriptor_buffer.size())) {
                identity->Product = TrimDescriptor(Utf16FromAnsi(
                    reinterpret_cast<const char*>(descriptor_buffer.data() + descriptor->ProductIdOffset)));
            }
        }
    }

    {
        GET_LENGTH_INFORMATION length_info{};

        if (DeviceIoControl(
                handle,
                IOCTL_DISK_GET_LENGTH_INFO,
                nullptr,
                0,
                &length_info,
                sizeof(length_info),
                &bytes_returned,
                nullptr)) {
            identity->LengthBytes = (uint64_t)length_info.Length.QuadPart;
        }
    }

    {
        STORAGE_DEVICE_NUMBER device_number{};

        if (DeviceIoControl(
                handle,
                IOCTL_STORAGE_GET_DEVICE_NUMBER,
                nullptr,
                0,
                &device_number,
                sizeof(device_number),
                &bytes_returned,
                nullptr)) {
            identity->DeviceNumber = device_number.DeviceNumber;
        }
    }

    CloseHandle(handle);
    return true;
}

bool IsTargetDiskCandidate(
    const DiskIdentity& identity)
{
    if (!ContainsInsensitive(identity.Vendor, L"Zightch")) {
        return false;
    }
    if (!ContainsInsensitive(identity.Product, L"YumeDisk")) {
        return false;
    }
    return true;
}

} // namespace

std::vector<DiskIdentity> EnumerateVisibleYumeDisks()
{
    std::vector<DiskIdentity> identities;
    const auto interfaces = EnumerateDeviceInterfaces(&GUID_DEVINTERFACE_DISK);

    for (const auto& path : interfaces) {
        DiskIdentity identity;

        if (QueryDiskIdentity(path, &identity) && IsTargetDiskCandidate(identity)) {
            identities.push_back(identity);
        }
    }

    std::sort(
        identities.begin(),
        identities.end(),
        [](const DiskIdentity& left, const DiskIdentity& right) {
            if (left.DeviceNumber != right.DeviceNumber) {
                return left.DeviceNumber < right.DeviceNumber;
            }
            return left.Path < right.Path;
        });
    return identities;
}

std::wstring MakePhysicalDrivePath(
    DWORD device_number)
{
    if (device_number == std::numeric_limits<DWORD>::max()) {
        return {};
    }

    return LR"(\\.\PhysicalDrive)" + std::to_wstring(device_number);
}

}
