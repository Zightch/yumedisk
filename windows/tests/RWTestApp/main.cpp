#define NOMINMAX

#include <Windows.h>
#include <SetupAPI.h>
#include <Ntddstor.h>
#include <WinIoCtl.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <cwctype>
#include <exception>
#include <iomanip>
#include <iostream>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <sstream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "appkernel.h"
#include "yumedisk_proto.h"

namespace {

constexpr ULONG kDefaultTargetId = 0;
constexpr ULONG kDefaultSectorSize = 4096;
constexpr size_t kDefaultQueueDepth = 32;
constexpr size_t kMaxSlotEngineQueueDepth = MAXIMUM_WAIT_OBJECTS / 2;
constexpr size_t kDefaultWriteSlotBytes = 1024 * 1024;
constexpr uint64_t kDefaultDiskSizeBytes = 64ull * 1024ull * 1024ull;
constexpr size_t kMaxReadWorkersPerDisk = 4;
constexpr size_t kMaxWriteWorkersPerDisk = 2;
constexpr size_t kReadSlotsPerWorkerTarget = 8;
constexpr size_t kWriteSlotsPerWorkerTarget = 16;
constexpr DWORD kDiskArrivalPollMs = 100;
constexpr DWORD kDiskArrivalTimeoutMs = 2000;
constexpr DWORD kEventWaitPollMs = 100;
constexpr UINT32 kHeartbeatIntervalMs = 1000;
constexpr UINT32 kInitialEventQueueCapacity = 1024;

struct AppConfig {
    ULONG targetId = kDefaultTargetId;
    ULONG sectorSize = kDefaultSectorSize;
    size_t queueDepth = kDefaultQueueDepth;
    size_t writeSlotBytes = kDefaultWriteSlotBytes;
    uint64_t diskSizeBytes = kDefaultDiskSizeBytes;
};

enum class ParseResult {
    Ok,
    Help,
    Error
};

struct DiskIdentity {
    std::wstring path;
    std::wstring vendor;
    std::wstring product;
    uint64_t lengthBytes = 0;
    DWORD deviceNumber = std::numeric_limits<DWORD>::max();
};

struct StagedFragment {
    UINT32 Seq = 0u;
    UINT64 DiskOffsetBytes = 0ull;
    UINT64 Ordinal = 0ull;
    std::vector<unsigned char> Data;
};

struct StagedWriteRecord {
    UINT32 TotalSeq = 0u;
    std::map<UINT32, StagedFragment> Fragments;
};

struct BackendContext;

struct ManagedDisk {
    BackendContext* Backend = nullptr;
    AK_DISK* Handle = nullptr;
    ULONG TargetId = 0u;
    ULONG SectorSize = 0u;
    uint64_t DiskSizeBytes = 0ull;
    size_t SlotDepth = 0u;
    size_t ReadWorkerCount = 0u;
    size_t WriteWorkerCount = 0u;
    DiskIdentity Identity{};
    mutable std::shared_mutex MediaLock;
    std::vector<unsigned char> Medium;
    std::map<UINT64, StagedWriteRecord> StagedWrites;
    UINT64 NextStageOrdinal = 1ull;
};

struct BackendContext {
    AppConfig Config;
    AK_SESSION* Session = nullptr;
    std::atomic<bool> Stop{false};
    std::mutex DisksLock;
    std::map<ULONG, std::shared_ptr<ManagedDisk>> Disks;
    std::mutex LogLock;
    std::thread EventThread;
};

HANDLE g_StopEvent = nullptr;

BOOL WINAPI ConsoleCtrlHandler(
    DWORD control_type)
{
    switch (control_type) {
    case CTRL_C_EVENT:
    case CTRL_BREAK_EVENT:
    case CTRL_CLOSE_EVENT:
    case CTRL_LOGOFF_EVENT:
    case CTRL_SHUTDOWN_EVENT:
        if (g_StopEvent != nullptr) {
            SetEvent(g_StopEvent);
        }
        return TRUE;

    default:
        return FALSE;
    }
}

void LogWide(
    BackendContext* context,
    const std::wstring& text)
{
    if (context == nullptr) {
        return;
    }

    std::lock_guard<std::mutex> guard(context->LogLock);
    std::wcerr << text << std::endl;
}

VOID AK_CALL AppKernelLogCallback(
    void* log_ctx,
    INT level,
    const char* text)
{
    BackendContext* context;
    std::ostringstream stream;

    context = static_cast<BackendContext*>(log_ctx);
    stream << "[AppKernel][" << level << "] " << (text == nullptr ? "" : text);

    std::lock_guard<std::mutex> guard(context->LogLock);
    std::cerr << stream.str() << std::endl;
}

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

    begin = 0u;
    end = text.size();
    while ((begin < end) && (iswspace(text[begin]) != 0)) {
        begin += 1u;
    }
    while ((end > begin) && (iswspace(text[end - 1u]) != 0)) {
        end -= 1u;
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

    for (start = 0u; start + needle.size() <= haystack.size(); ++start) {
        bool match;

        match = true;
        for (index = 0u; index < needle.size(); ++index) {
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

    for (index = 0u;; ++index) {
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

        required_size = 0u;
        (void)SetupDiGetDeviceInterfaceDetailW(
            info,
            &interface_data,
            nullptr,
            0u,
            &required_size,
            nullptr);
        if (required_size == 0u) {
            continue;
        }

        detail_buffer.resize(required_size, 0u);
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

    identity->path = path;
    identity->vendor.clear();
    identity->product.clear();
    identity->lengthBytes = 0ull;
    identity->deviceNumber = std::numeric_limits<DWORD>::max();

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
    bytes_returned = 0u;

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

        descriptor_buffer.resize(header.Size, 0u);
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
            if ((descriptor->VendorIdOffset != 0u) &&
                (descriptor->VendorIdOffset < descriptor_buffer.size())) {
                identity->vendor = TrimDescriptor(Utf16FromAnsi(
                    reinterpret_cast<const char*>(descriptor_buffer.data() + descriptor->VendorIdOffset)));
            }
            if ((descriptor->ProductIdOffset != 0u) &&
                (descriptor->ProductIdOffset < descriptor_buffer.size())) {
                identity->product = TrimDescriptor(Utf16FromAnsi(
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
                0u,
                &length_info,
                sizeof(length_info),
                &bytes_returned,
                nullptr)) {
            identity->lengthBytes = (uint64_t)length_info.Length.QuadPart;
        }
    }

    {
        STORAGE_DEVICE_NUMBER device_number{};

        if (DeviceIoControl(
                handle,
                IOCTL_STORAGE_GET_DEVICE_NUMBER,
                nullptr,
                0u,
                &device_number,
                sizeof(device_number),
                &bytes_returned,
                nullptr)) {
            identity->deviceNumber = device_number.DeviceNumber;
        }
    }

    CloseHandle(handle);
    return true;
}

bool IsTargetDiskCandidate(
    const DiskIdentity& identity,
    const AppConfig& config)
{
    if (identity.lengthBytes != config.diskSizeBytes) {
        return false;
    }
    if (!ContainsInsensitive(identity.vendor, L"Zightch")) {
        return false;
    }
    if (!ContainsInsensitive(identity.product, L"YumeDisk")) {
        return false;
    }
    return true;
}

std::vector<DiskIdentity> EnumerateVisibleYumeDisks(
    const AppConfig& config)
{
    std::vector<DiskIdentity> identities;
    const auto interfaces = EnumerateDeviceInterfaces(&GUID_DEVINTERFACE_DISK);

    for (const auto& path : interfaces) {
        DiskIdentity identity;

        if (QueryDiskIdentity(path, &identity) && IsTargetDiskCandidate(identity, config)) {
            identities.push_back(identity);
        }
    }

    std::sort(
        identities.begin(),
        identities.end(),
        [](const DiskIdentity& left, const DiskIdentity& right) {
            if (left.deviceNumber != right.deviceNumber) {
                return left.deviceNumber < right.deviceNumber;
            }
            return left.path < right.path;
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

std::vector<std::shared_ptr<ManagedDisk>> SnapshotManagedDisks(
    BackendContext* context)
{
    std::vector<std::shared_ptr<ManagedDisk>> disks;

    if (context == nullptr) {
        return disks;
    }

    std::lock_guard<std::mutex> guard(context->DisksLock);
    disks.reserve(context->Disks.size());
    for (const auto& entry : context->Disks) {
        disks.push_back(entry.second);
    }

    return disks;
}

std::shared_ptr<ManagedDisk> FindManagedDisk(
    BackendContext* context,
    ULONG target_id)
{
    std::lock_guard<std::mutex> guard(context->DisksLock);
    const auto it = context->Disks.find(target_id);
    if (it == context->Disks.end()) {
        return nullptr;
    }

    return it->second;
}

void InsertManagedDisk(
    BackendContext* context,
    const std::shared_ptr<ManagedDisk>& disk)
{
    std::lock_guard<std::mutex> guard(context->DisksLock);
    context->Disks[disk->TargetId] = disk;
}

void RemoveManagedDiskFromMap(
    BackendContext* context,
    ULONG target_id)
{
    std::lock_guard<std::mutex> guard(context->DisksLock);
    context->Disks.erase(target_id);
}

bool ManagedDiskExists(
    BackendContext* context,
    ULONG target_id)
{
    std::lock_guard<std::mutex> guard(context->DisksLock);
    return context->Disks.find(target_id) != context->Disks.end();
}

ULONG FindFirstFreeTarget(
    BackendContext* context)
{
    std::lock_guard<std::mutex> guard(context->DisksLock);

    for (ULONG target_id = YUMEDISK_MIN_TARGET_ID;
         target_id <= YUMEDISK_MAX_USABLE_TARGET_ID;
         ++target_id) {
        if (context->Disks.find(target_id) == context->Disks.end()) {
            return target_id;
        }
    }

    return YUMEDISK_MAX_TARGETS;
}

std::vector<std::wstring> SnapshotClaimedDiskPaths(
    BackendContext* context,
    ULONG excluded_target_id)
{
    std::vector<std::wstring> paths;

    std::lock_guard<std::mutex> guard(context->DisksLock);
    for (const auto& entry : context->Disks) {
        if (entry.first == excluded_target_id) {
            continue;
        }
        if (!entry.second->Identity.path.empty()) {
            paths.push_back(entry.second->Identity.path);
        }
    }

    return paths;
}

bool ContainsPath(
    const std::vector<std::wstring>& paths,
    const std::wstring& path)
{
    return std::find(paths.begin(), paths.end(), path) != paths.end();
}

bool ContainsVisibleDiskPath(
    const std::vector<DiskIdentity>& identities,
    const std::wstring& path)
{
    return std::any_of(
        identities.begin(),
        identities.end(),
        [&](const DiskIdentity& identity) {
            return identity.path == path;
        });
}

bool TryRefreshManagedDiskIdentity(
    BackendContext* context,
    const std::shared_ptr<ManagedDisk>& disk,
    const std::vector<DiskIdentity>* baseline_visible_disks,
    DWORD timeout_ms)
{
    const auto claimed_paths = SnapshotClaimedDiskPaths(context, disk->TargetId);
    const ULONGLONG start_tick = GetTickCount64();

    for (;;) {
        const auto visible_disks = EnumerateVisibleYumeDisks(context->Config);
        const DiskIdentity* selected = nullptr;

        for (const auto& identity : visible_disks) {
            if (ContainsPath(claimed_paths, identity.path)) {
                continue;
            }
            if ((baseline_visible_disks != nullptr) &&
                ContainsVisibleDiskPath(*baseline_visible_disks, identity.path)) {
                continue;
            }

            selected = &identity;
            break;
        }

        if ((selected == nullptr) && !visible_disks.empty()) {
            for (const auto& identity : visible_disks) {
                if (!ContainsPath(claimed_paths, identity.path)) {
                    selected = &identity;
                    break;
                }
            }
        }

        if (selected != nullptr) {
            disk->Identity = *selected;
            return true;
        }

        if ((timeout_ms == 0u) ||
            context->Stop.load(std::memory_order_relaxed) ||
            ((GetTickCount64() - start_tick) >= timeout_ms)) {
            return false;
        }

        Sleep(kDiskArrivalPollMs);
    }
}

size_t ComputeWorkerCount(
    size_t slot_depth,
    size_t target_slots_per_worker,
    size_t max_workers)
{
    size_t worker_count;

    if (slot_depth == 0u) {
        return 1u;
    }

    worker_count = (slot_depth + target_slots_per_worker - 1u) / target_slots_per_worker;
    worker_count = std::max<size_t>(1u, worker_count);
    worker_count = std::min(worker_count, max_workers);
    worker_count = std::min(worker_count, slot_depth);
    return worker_count;
}

std::wstring FormatStatusHex(
    AK_STATUS status)
{
    std::wostringstream stream;

    stream << L"0x"
           << std::uppercase
           << std::hex
           << std::setw(8)
           << std::setfill(L'0')
           << (unsigned long)status;
    return stream.str();
}

std::wstring LifecycleToText(
    AK_LIFECYCLE_STATE lifecycle)
{
    switch (lifecycle) {
    case AkStateInit:
        return L"init";
    case AkStateStarting:
        return L"starting";
    case AkStateRunning:
        return L"running";
    case AkStateRemoving:
        return L"removing";
    case AkStateClosing:
        return L"closing";
    case AkStateClosed:
        return L"closed";
    case AkStateBroken:
        return L"broken";
    default:
        return L"unknown";
    }
}

size_t CountStagedFragmentsLocked(
    const ManagedDisk* disk)
{
    size_t count;

    count = 0u;
    for (const auto& entry : disk->StagedWrites) {
        count += entry.second.Fragments.size();
    }
    return count;
}

AK_STATUS AK_CALL HostReadBytes(
    void* media_ctx,
    const AK_READ_OP* op,
    void* out_buffer,
    UINT32* out_data_length)
{
    ManagedDisk* disk;
    unsigned char* buffer;
    UINT64 request_begin;
    UINT64 request_end;

    disk = static_cast<ManagedDisk*>(media_ctx);
    if ((disk == nullptr) || (op == nullptr) || (out_buffer == nullptr) || (out_data_length == nullptr)) {
        return AK_STATUS_INVALID_PARAMETER;
    }

    if (op->DataLength == 0u) {
        *out_data_length = 0u;
        return AK_STATUS_SUCCESS;
    }

    request_begin = op->OffsetBytes;
    request_end = request_begin + (UINT64)op->DataLength;
    if ((request_end < request_begin) || (request_end > disk->DiskSizeBytes)) {
        *out_data_length = 0u;
        return AK_STATUS_INVALID_PARAMETER;
    }

    buffer = static_cast<unsigned char*>(out_buffer);
    {
        struct OverlaySlice {
            UINT64 Ordinal;
            size_t DestOffset;
            size_t SourceOffset;
            size_t Length;
            const unsigned char* Data;
        };
        std::vector<OverlaySlice> overlays;

        std::shared_lock<std::shared_mutex> guard(disk->MediaLock);
        (void)memcpy(
            buffer,
            disk->Medium.data() + (size_t)request_begin,
            op->DataLength);

        for (const auto& staged_entry : disk->StagedWrites) {
            for (const auto& fragment_entry : staged_entry.second.Fragments) {
                const StagedFragment& fragment = fragment_entry.second;
                UINT64 fragment_begin;
                UINT64 fragment_end;
                UINT64 overlap_begin;
                UINT64 overlap_end;
                OverlaySlice slice;

                fragment_begin = fragment.DiskOffsetBytes;
                fragment_end = fragment_begin + (UINT64)fragment.Data.size();
                if ((fragment_end <= request_begin) || (fragment_begin >= request_end)) {
                    continue;
                }

                overlap_begin = std::max(fragment_begin, request_begin);
                overlap_end = std::min(fragment_end, request_end);
                if (overlap_end <= overlap_begin) {
                    continue;
                }

                slice.Ordinal = fragment.Ordinal;
                slice.DestOffset = (size_t)(overlap_begin - request_begin);
                slice.SourceOffset = (size_t)(overlap_begin - fragment_begin);
                slice.Length = (size_t)(overlap_end - overlap_begin);
                slice.Data = fragment.Data.data();
                overlays.push_back(slice);
            }
        }

        std::sort(
            overlays.begin(),
            overlays.end(),
            [](const OverlaySlice& left, const OverlaySlice& right) {
                return left.Ordinal < right.Ordinal;
            });

        for (const auto& slice : overlays) {
            (void)memcpy(
                buffer + slice.DestOffset,
                slice.Data + slice.SourceOffset,
                slice.Length);
        }
    }

    *out_data_length = op->DataLength;
    return AK_STATUS_SUCCESS;
}

AK_STATUS AK_CALL HostStageWrite(
    void* media_ctx,
    const AK_WRITE_OP* op,
    const void* data_buffer,
    UINT32 data_length)
{
    ManagedDisk* disk;
    UINT64 write_begin;
    UINT64 write_end;

    disk = static_cast<ManagedDisk*>(media_ctx);
    if ((disk == nullptr) || (op == nullptr)) {
        return AK_STATUS_INVALID_PARAMETER;
    }

    if (data_length != op->DataLength) {
        return AK_STATUS_INVALID_PARAMETER;
    }

    write_begin = op->OffsetBytes;
    write_end = write_begin + (UINT64)data_length;
    if ((write_end < write_begin) || (write_end > disk->DiskSizeBytes)) {
        return AK_STATUS_INVALID_PARAMETER;
    }

    {
        std::unique_lock<std::shared_mutex> guard(disk->MediaLock);
        StagedWriteRecord& record = disk->StagedWrites[op->EventId];
        StagedFragment fragment;

        if ((record.TotalSeq != 0u) && (record.TotalSeq != op->TotalSeq)) {
            return AK_STATUS_INVALID_PARAMETER;
        }

        record.TotalSeq = op->TotalSeq;
        fragment.Seq = op->Seq;
        fragment.DiskOffsetBytes = op->OffsetBytes;
        fragment.Ordinal = disk->NextStageOrdinal;
        disk->NextStageOrdinal += 1ull;
        fragment.Data.resize(data_length, 0u);
        if ((data_length != 0u) && (data_buffer != nullptr)) {
            (void)memcpy(fragment.Data.data(), data_buffer, data_length);
        }

        record.Fragments[op->Seq] = std::move(fragment);
    }

    return AK_STATUS_SUCCESS;
}

bool ApplyCommittedWrite(
    ManagedDisk* disk,
    UINT64 event_id)
{
    std::unique_lock<std::shared_mutex> guard(disk->MediaLock);
    const auto it = disk->StagedWrites.find(event_id);
    if (it == disk->StagedWrites.end()) {
        return true;
    }

    for (const auto& fragment_entry : it->second.Fragments) {
        const StagedFragment& fragment = fragment_entry.second;
        const UINT64 end_offset = fragment.DiskOffsetBytes + (UINT64)fragment.Data.size();

        if ((end_offset < fragment.DiskOffsetBytes) || (end_offset > disk->DiskSizeBytes)) {
            return false;
        }

        if (!fragment.Data.empty()) {
            (void)memcpy(
                disk->Medium.data() + (size_t)fragment.DiskOffsetBytes,
                fragment.Data.data(),
                fragment.Data.size());
        }
    }

    disk->StagedWrites.erase(it);
    return true;
}

void DiscardStagedWrite(
    ManagedDisk* disk,
    UINT64 event_id)
{
    std::unique_lock<std::shared_mutex> guard(disk->MediaLock);
    disk->StagedWrites.erase(event_id);
}

void HandleAppKernelEvent(
    BackendContext* context,
    const AK_EVENT* event_record)
{
    std::shared_ptr<ManagedDisk> disk;

    if ((context == nullptr) || (event_record == nullptr)) {
        return;
    }

    if (event_record->Type == AkEventSessionBroken) {
        LogWide(
            context,
            L"session broken, status=" + FormatStatusHex(event_record->Status));
        context->Stop.store(true, std::memory_order_relaxed);
        if (g_StopEvent != nullptr) {
            SetEvent(g_StopEvent);
        }
        return;
    }

    disk = FindManagedDisk(context, event_record->TargetId);
    if (disk == nullptr) {
        return;
    }

    switch (event_record->Type) {
    case AkEventDiskOnline:
        (void)TryRefreshManagedDiskIdentity(context, disk, nullptr, 0u);
        break;

    case AkEventDiskRemoved:
        break;

    case AkEventWriteFinalCommitted:
        if (!ApplyCommittedWrite(disk.get(), event_record->EventId)) {
            LogWide(
                context,
                L"commit write failed, target=" + std::to_wstring(event_record->TargetId) +
                    L", event=" + std::to_wstring(event_record->EventId));
        }
        break;

    case AkEventWriteFinalRejected:
        DiscardStagedWrite(disk.get(), event_record->EventId);
        break;

    default:
        break;
    }
}

void RunEventLoop(
    BackendContext* context)
{
    while (!context->Stop.load(std::memory_order_relaxed)) {
        AK_EVENT event_record{};
        AK_STATUS status;

        status = AkWaitEvent(context->Session, kEventWaitPollMs, &event_record);
        if (status == AK_STATUS_TIMEOUT) {
            continue;
        }
        if (status == AK_STATUS_NO_MORE_ENTRIES) {
            continue;
        }
        if (status != AK_STATUS_SUCCESS) {
            LogWide(context, L"event loop failed, status=" + FormatStatusHex(status));
            context->Stop.store(true, std::memory_order_relaxed);
            if (g_StopEvent != nullptr) {
                SetEvent(g_StopEvent);
            }
            break;
        }

        HandleAppKernelEvent(context, &event_record);
    }
}

void PrintRuntimeHelp()
{
    std::cout
        << "commands:\n"
        << "  help           show this help\n"
        << "  query          print AppKernel session state\n"
        << "  ct [target]    create one disk target through AppKernel\n"
        << "  rm <target>    remove one disk target\n"
        << "  rm all         remove all disk targets\n"
        << "  ls             list managed targets and visible YumeDisk disks\n"
        << "  stats          print aggregated AppKernel counters\n"
        << "  debug          print app host plus AppKernel snapshot\n"
        << "  exit           close session and quit\n";
}

void PrintUsage()
{
    std::cout
        << "RWTestApp [--queue-depth N] [--slot-bytes BYTES] [--disk-size-mb MB]\n"
        << "          [--sector-size BYTES] [--target ID]\n"
        << "\n"
        << "defaults:\n"
        << "  queue-depth = " << kDefaultQueueDepth << "\n"
        << "  slot-bytes  = " << kDefaultWriteSlotBytes << "\n"
        << "  disk-size-mb = " << (kDefaultDiskSizeBytes / (1024ull * 1024ull)) << "\n"
        << "  sector-size = " << kDefaultSectorSize << "\n"
        << "  target      = " << kDefaultTargetId << "\n"
        << "  queue-depth max = " << kMaxSlotEngineQueueDepth << "\n";
}

bool ParseUnsigned(
    const char* text,
    uint64_t* value)
{
    char* end;
    uint64_t parsed;

    end = nullptr;
    parsed = _strtoui64(text, &end, 0);
    if ((end == text) || (*end != '\0')) {
        return false;
    }

    *value = parsed;
    return true;
}

ParseResult ParseArgs(
    int argc,
    char** argv,
    AppConfig* config)
{
    int index;

    for (index = 1; index < argc; ++index) {
        const std::string arg = argv[index];
        uint64_t value;
        auto next_value = [&](uint64_t* out) -> bool {
            if ((index + 1) >= argc) {
                return false;
            }
            index += 1;
            return ParseUnsigned(argv[index], out);
        };

        value = 0ull;
        if ((arg == "--help") || (arg == "-h")) {
            return ParseResult::Help;
        }
        if (arg == "--queue-depth") {
            if (!next_value(&value) || (value == 0ull) ||
                (value > std::numeric_limits<size_t>::max()) ||
                (value > kMaxSlotEngineQueueDepth)) {
                return ParseResult::Error;
            }
            config->queueDepth = (size_t)value;
            continue;
        }
        if (arg == "--slot-bytes") {
            if (!next_value(&value) || (value == 0ull) ||
                (value > std::numeric_limits<size_t>::max())) {
                return ParseResult::Error;
            }
            config->writeSlotBytes = (size_t)value;
            continue;
        }
        if (arg == "--disk-size-mb") {
            if (!next_value(&value) || (value == 0ull)) {
                return ParseResult::Error;
            }
            config->diskSizeBytes = value * 1024ull * 1024ull;
            continue;
        }
        if (arg == "--sector-size") {
            if (!next_value(&value) || (value == 0ull) ||
                (value > std::numeric_limits<ULONG>::max())) {
                return ParseResult::Error;
            }
            config->sectorSize = (ULONG)value;
            continue;
        }
        if (arg == "--target") {
            if (!next_value(&value) || (value > YUMEDISK_MAX_USABLE_TARGET_ID)) {
                return ParseResult::Error;
            }
            config->targetId = (ULONG)value;
            continue;
        }

        return ParseResult::Error;
    }

    if ((config->queueDepth == 0u) ||
        (config->writeSlotBytes < config->sectorSize) ||
        ((config->writeSlotBytes % config->sectorSize) != 0u) ||
        (config->diskSizeBytes == 0ull) ||
        ((config->diskSizeBytes % config->sectorSize) != 0ull)) {
        return ParseResult::Error;
    }

    return ParseResult::Ok;
}

bool ParseTargetToken(
    const std::string& token,
    ULONG* target_id)
{
    uint64_t value;

    value = 0ull;
    if (!ParseUnsigned(token.c_str(), &value) || (value > YUMEDISK_MAX_USABLE_TARGET_ID)) {
        return false;
    }

    *target_id = (ULONG)value;
    return true;
}

void PrintBackendStats(
    BackendContext* context)
{
    AK_SESSION_STATS session_stats{};
    AK_STATUS session_status;
    UINT64 read_slot_posts;
    UINT64 read_slot_completions;
    UINT64 read_ack_commands;
    UINT64 write_slot_posts;
    UINT64 write_slot_completions;
    UINT64 write_ack_flushes;
    UINT64 write_ack_ranges;
    UINT64 write_ack_range_failures;
    UINT64 final_write_committed;
    UINT64 final_write_rejected;

    session_status = AkQuerySessionStats(context->Session, &session_stats);
    if (session_status != AK_STATUS_SUCCESS) {
        std::wcout << L"stats_query_failed=true, status=" << FormatStatusHex(session_status) << std::endl;
        return;
    }

    read_slot_posts = 0ull;
    read_slot_completions = 0ull;
    read_ack_commands = 0ull;
    write_slot_posts = 0ull;
    write_slot_completions = 0ull;
    write_ack_flushes = 0ull;
    write_ack_ranges = 0ull;
    write_ack_range_failures = 0ull;
    final_write_committed = 0ull;
    final_write_rejected = 0ull;

    for (const auto& disk : SnapshotManagedDisks(context)) {
        AK_DISK_STATS disk_stats{};
        if (disk->Handle == nullptr) {
            continue;
        }
        if (AkQueryDiskStats(disk->Handle, &disk_stats) != AK_STATUS_SUCCESS) {
            continue;
        }

        read_slot_posts += disk_stats.ReadSlotPosts;
        read_slot_completions += disk_stats.ReadSlotCompletions;
        read_ack_commands += disk_stats.ReadAckCommands;
        write_slot_posts += disk_stats.WriteSlotPosts;
        write_slot_completions += disk_stats.WriteSlotCompletions;
        write_ack_flushes += disk_stats.WriteAckFlushes;
        write_ack_ranges += disk_stats.WriteAckRanges;
        write_ack_range_failures += disk_stats.WriteAckRangeFailures;
        final_write_committed += disk_stats.FinalWriteCommitted;
        final_write_rejected += disk_stats.FinalWriteRejected;
    }

    std::wcout << L"backend_read_slot_posts=" << read_slot_posts
               << L", backend_read_slot_completions=" << read_slot_completions
               << L", backend_read_ack_commands=" << read_ack_commands
               << L", backend_write_slot_posts=" << write_slot_posts
               << L", backend_write_slot_completions=" << write_slot_completions
               << L", backend_flushed_ack_commands=" << write_ack_flushes
               << L", backend_flushed_ack_ranges=" << write_ack_ranges
               << L", backend_write_ack_range_failures=" << write_ack_range_failures
               << L", backend_final_write_committed=" << final_write_committed
               << L", backend_final_write_rejected=" << final_write_rejected
               << L", backend_command_failures=" << session_stats.CommandFailures
               << L", backend_protocol_failures=" << session_stats.ProtocolFailures
               << std::endl;
}

void PrintDebugSnapshot(
    BackendContext* context,
    const wchar_t* reason)
{
    AK_SESSION_STATE session_state{};
    const auto disks = SnapshotManagedDisks(context);

    std::wcout << L"debug_snapshot reason=" << reason << std::endl;
    PrintBackendStats(context);

    if (AkQuerySessionState(context->Session, &session_state) == AK_STATUS_SUCCESS) {
        std::wcout << L"debug_session session=" << session_state.SessionId
                   << L", lifecycle=" << LifecycleToText(session_state.Lifecycle)
                   << L", heartbeat_running=" << (session_state.HeartbeatRunning ? L"true" : L"false")
                   << L", transport_ready=" << (session_state.TransportReady ? L"true" : L"false")
                   << L", disk_count=" << session_state.DiskCount
                   << L", last_error=" << FormatStatusHex(session_state.LastError)
                   << std::endl;
    }

    for (const auto& disk : disks) {
        AK_DISK_STATE disk_state{};
        size_t staged_write_count;
        size_t staged_fragment_count;

        {
            std::shared_lock<std::shared_mutex> guard(disk->MediaLock);
            staged_write_count = disk->StagedWrites.size();
            staged_fragment_count = CountStagedFragmentsLocked(disk.get());
        }

        if ((disk->Handle != nullptr) &&
            (AkQueryDiskState(disk->Handle, &disk_state) == AK_STATUS_SUCCESS)) {
            std::wcout << L"debug_disk target=" << disk->TargetId
                       << L", lifecycle=" << LifecycleToText(disk_state.Lifecycle)
                       << L", read_workers_running=" << (disk_state.ReadWorkersRunning ? L"true" : L"false")
                       << L", write_workers_running=" << (disk_state.WriteWorkersRunning ? L"true" : L"false")
                       << L", ack_flusher_running=" << (disk_state.AckFlusherRunning ? L"true" : L"false")
                       << L", read_workers=" << disk->ReadWorkerCount
                       << L", write_workers=" << disk->WriteWorkerCount
                       << L", staged_writes=" << staged_write_count
                       << L", staged_fragments=" << staged_fragment_count
                       << L", visible_path=" << (disk->Identity.path.empty() ? L"<pending-enumeration>" : disk->Identity.path)
                       << std::endl;
        } else {
            std::wcout << L"debug_disk target=" << disk->TargetId
                       << L", lifecycle=<unknown>"
                       << L", read_workers=" << disk->ReadWorkerCount
                       << L", write_workers=" << disk->WriteWorkerCount
                       << L", staged_writes=" << staged_write_count
                       << L", staged_fragments=" << staged_fragment_count
                       << L", visible_path=" << (disk->Identity.path.empty() ? L"<pending-enumeration>" : disk->Identity.path)
                       << std::endl;
        }
    }
}

void RunQuery(
    BackendContext* context)
{
    AK_SESSION_STATE session_state{};
    AK_SESSION_STATS session_stats{};
    AK_STATUS status;

    status = AkQuerySessionState(context->Session, &session_state);
    if (status != AK_STATUS_SUCCESS) {
        std::wcerr << L"query session state failed, status=" << FormatStatusHex(status) << std::endl;
        return;
    }

    status = AkQuerySessionStats(context->Session, &session_stats);
    if (status != AK_STATUS_SUCCESS) {
        std::wcerr << L"query session stats failed, status=" << FormatStatusHex(status) << std::endl;
        return;
    }

    std::wcout << L"session=" << session_state.SessionId
               << L", lifecycle=" << LifecycleToText(session_state.Lifecycle)
               << L", heartbeat_running=" << (session_state.HeartbeatRunning ? L"true" : L"false")
               << L", transport_ready=" << (session_state.TransportReady ? L"true" : L"false")
               << L", disk_count=" << session_state.DiskCount
               << L", last_error=" << FormatStatusHex(session_state.LastError)
               << std::endl;
    std::wcout << L"session_heartbeat_sent=" << session_stats.HeartbeatSent
               << L", session_command_failures=" << session_stats.CommandFailures
               << L", session_protocol_failures=" << session_stats.ProtocolFailures
               << L", session_events_queued=" << session_stats.EventsQueued
               << L", session_events_dropped=" << session_stats.EventsDropped
               << std::endl;
}

void ListManagedDisks(
    BackendContext* context)
{
    const auto managed_disks = SnapshotManagedDisks(context);
    const auto visible_disks = EnumerateVisibleYumeDisks(context->Config);

    std::wcout << L"managed_target_count=" << managed_disks.size() << std::endl;
    for (const auto& disk : managed_disks) {
        AK_DISK_STATE disk_state{};
        const std::wstring physical_drive = MakePhysicalDrivePath(disk->Identity.deviceNumber);
        const bool have_state = (disk->Handle != nullptr) &&
            (AkQueryDiskState(disk->Handle, &disk_state) == AK_STATUS_SUCCESS);

        (void)TryRefreshManagedDiskIdentity(context, disk, nullptr, 0u);
        std::wcout << L"target=" << disk->TargetId
                   << L", disk_bytes=" << disk->DiskSizeBytes
                   << L", sector_size=" << disk->SectorSize
                   << L", slot_engine=" << ((have_state && (disk_state.Lifecycle == AkStateRunning)) ? L"running" : L"stopped")
                   << L", read_workers=" << disk->ReadWorkerCount
                   << L", write_workers=" << disk->WriteWorkerCount
                   << L", read_slot_depth=" << disk->SlotDepth
                   << L", write_slot_depth=" << disk->SlotDepth
                   << L", visible_path=" << (disk->Identity.path.empty() ? L"<pending-enumeration>" : disk->Identity.path)
                   << L", physical_drive=" << (physical_drive.empty() ? L"<pending-enumeration>" : physical_drive)
                   << std::endl;
    }

    std::wcout << L"visible_disk_count=" << visible_disks.size() << std::endl;
    for (size_t index = 0u; index < visible_disks.size(); ++index) {
        const auto& disk = visible_disks[index];
        std::wcout << L"visible_disk[" << index << L"]"
                   << L", path=" << disk.path
                   << L", device_number=" << disk.deviceNumber
                   << L", disk_bytes=" << disk.lengthBytes
                   << std::endl;
    }
}

bool CreateManagedDisk(
    BackendContext* context,
    ULONG target_id)
{
    std::shared_ptr<ManagedDisk> disk;
    AK_DISK_PARAMS params{};
    AK_DISK* handle;
    AK_STATUS status;
    const auto visible_disks_before_create = EnumerateVisibleYumeDisks(context->Config);
    static const AK_MEDIA_OPS media_ops = {
        HostReadBytes,
        HostStageWrite
    };

    if (ManagedDiskExists(context, target_id)) {
        std::wcerr << L"create failed, target already exists: " << target_id << std::endl;
        return false;
    }

    try {
        disk = std::make_shared<ManagedDisk>();
        disk->Backend = context;
        disk->TargetId = target_id;
        disk->SectorSize = context->Config.sectorSize;
        disk->DiskSizeBytes = context->Config.diskSizeBytes;
        disk->SlotDepth = context->Config.queueDepth;
        disk->ReadWorkerCount = ComputeWorkerCount(
            disk->SlotDepth,
            kReadSlotsPerWorkerTarget,
            kMaxReadWorkersPerDisk);
        disk->WriteWorkerCount = ComputeWorkerCount(
            disk->SlotDepth,
            kWriteSlotsPerWorkerTarget,
            kMaxWriteWorkersPerDisk);
        disk->Medium.resize((size_t)context->Config.diskSizeBytes, 0u);
    } catch (const std::exception&) {
        std::wcerr << L"create failed, target=" << target_id << L", reason=memory-allocation" << std::endl;
        return false;
    }

    InsertManagedDisk(context, disk);

    params.TargetId = target_id;
    params.SectorSize = context->Config.sectorSize;
    params.DiskSizeBytes = context->Config.diskSizeBytes;
    params.QueueDepth = (UINT32)context->Config.queueDepth;
    params.WriteSlotBytes = (UINT32)context->Config.writeSlotBytes;
    params.ReadWorkerCount = (UINT16)disk->ReadWorkerCount;
    params.WriteWorkerCount = (UINT16)disk->WriteWorkerCount;
    params.AckBatchMaxRanges = (UINT32)context->Config.queueDepth;

    handle = nullptr;
    status = AkCreateDisk(context->Session, &params, &media_ops, disk.get(), &handle);
    if (status != AK_STATUS_SUCCESS) {
        RemoveManagedDiskFromMap(context, target_id);
        std::wcerr << L"create failed, target=" << target_id
                   << L", status=" << FormatStatusHex(status) << std::endl;
        return false;
    }

    disk->Handle = handle;
    std::wcout << L"created target=" << target_id
               << L", queue_depth=" << context->Config.queueDepth
               << L", slot_bytes=" << context->Config.writeSlotBytes
               << std::endl;
    if (TryRefreshManagedDiskIdentity(context, disk, &visible_disks_before_create, kDiskArrivalTimeoutMs)) {
        std::wcout << L"visible_path=" << disk->Identity.path
                   << L", physical_drive=" << MakePhysicalDrivePath(disk->Identity.deviceNumber)
                   << std::endl;
    } else {
        std::wcout << L"visible_path=<pending-enumeration>, target=" << target_id << std::endl;
    }

    return true;
}

bool RemoveManagedDisk(
    BackendContext* context,
    ULONG target_id)
{
    const auto disk = FindManagedDisk(context, target_id);
    AK_STATUS status;

    if (disk == nullptr) {
        std::wcerr << L"remove failed, target not found: " << target_id << std::endl;
        return false;
    }

    if (disk->Handle == nullptr) {
        RemoveManagedDiskFromMap(context, target_id);
        std::wcout << L"removed target=" << target_id << std::endl;
        return true;
    }

    status = AkRemoveDisk(disk->Handle);
    if (status != AK_STATUS_SUCCESS) {
        std::wcerr << L"remove failed, target=" << target_id
                   << L", status=" << FormatStatusHex(status) << std::endl;
        return false;
    }

    disk->Handle = nullptr;
    RemoveManagedDiskFromMap(context, target_id);
    std::wcout << L"removed target=" << target_id << std::endl;
    return true;
}

bool RemoveAllManagedDisks(
    BackendContext* context,
    bool closing)
{
    const auto disks = SnapshotManagedDisks(context);
    bool ok;

    ok = true;
    for (const auto& disk : disks) {
        if ((disk == nullptr) || (disk->Handle == nullptr)) {
            continue;
        }

        if (AkRemoveDisk(disk->Handle) != AK_STATUS_SUCCESS) {
            if (!closing) {
                std::wcerr << L"remove all failed, target=" << disk->TargetId << std::endl;
            }
            ok = false;
        }
        disk->Handle = nullptr;
    }

    {
        std::lock_guard<std::mutex> guard(context->DisksLock);
        context->Disks.clear();
    }

    std::wcout << L"removed_all=true" << std::endl;
    return ok || closing;
}

void RunCommandLoop(
    BackendContext* context)
{
    PrintRuntimeHelp();

    while (!context->Stop.load(std::memory_order_relaxed)) {
        std::cout << "> " << std::flush;

        std::string line;
        std::istringstream input;
        std::string command;

        if (!std::getline(std::cin, line)) {
            if (g_StopEvent != nullptr) {
                SetEvent(g_StopEvent);
            }
            break;
        }

        input.str(line);
        input >> command;
        if (command.empty()) {
            continue;
        }

        if (command == "help") {
            PrintRuntimeHelp();
            continue;
        }
        if (command == "query") {
            RunQuery(context);
            continue;
        }
        if (command == "ls") {
            ListManagedDisks(context);
            continue;
        }
        if (command == "stats") {
            PrintBackendStats(context);
            continue;
        }
        if (command == "debug") {
            PrintDebugSnapshot(context, L"manual");
            continue;
        }
        if ((command == "exit") || (command == "quit")) {
            if (g_StopEvent != nullptr) {
                SetEvent(g_StopEvent);
            }
            break;
        }
        if (command == "ct") {
            std::string arg;
            ULONG target_id;

            target_id = context->Config.targetId;
            if (input >> arg) {
                if (!ParseTargetToken(arg, &target_id)) {
                    std::wcerr << L"invalid target: " << std::wstring(arg.begin(), arg.end()) << std::endl;
                    continue;
                }
            } else {
                target_id = FindFirstFreeTarget(context);
                if (target_id >= YUMEDISK_MAX_TARGETS) {
                    std::wcerr << L"no free target" << std::endl;
                    continue;
                }
            }

            (void)CreateManagedDisk(context, target_id);
            continue;
        }
        if (command == "rm") {
            std::string arg;
            ULONG target_id;

            if (!(input >> arg)) {
                std::wcerr << L"rm requires <target>|all" << std::endl;
                continue;
            }

            if (arg == "all") {
                (void)RemoveAllManagedDisks(context, false);
                continue;
            }

            target_id = 0u;
            if (!ParseTargetToken(arg, &target_id)) {
                std::wcerr << L"invalid target: " << std::wstring(arg.begin(), arg.end()) << std::endl;
                continue;
            }

            (void)RemoveManagedDisk(context, target_id);
            continue;
        }

        std::wcerr << L"unknown command: " << std::wstring(command.begin(), command.end()) << std::endl;
    }
}

} // namespace

int main(
    int argc,
    char** argv)
{
    AppConfig config;
    ParseResult parse_result;
    BackendContext backend{};
    AK_OPEN_PARAMS open_params{};
    AK_STATUS status;
    AK_SESSION_STATE session_state{};
    AK_SESSION_STATS session_stats{};
    bool ok;

    parse_result = ParseArgs(argc, argv, &config);
    if (parse_result == ParseResult::Help) {
        PrintUsage();
        return 0;
    }
    if (parse_result != ParseResult::Ok) {
        PrintUsage();
        return 1;
    }

    backend.Config = config;

    g_StopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (g_StopEvent == nullptr) {
        std::cerr << "create stop event failed, error=" << GetLastError() << std::endl;
        return 1;
    }

    (void)SetConsoleCtrlHandler(ConsoleCtrlHandler, TRUE);

    open_params.HeartbeatIntervalMs = kHeartbeatIntervalMs;
    open_params.InitialEventQueueCapacity = kInitialEventQueueCapacity;
    open_params.LogFn = AppKernelLogCallback;
    open_params.LogCtx = &backend;

    status = AkOpen(&open_params, &backend.Session);
    if (status != AK_STATUS_SUCCESS) {
        std::cerr << "open appkernel session failed, status="
                  << std::hex << std::uppercase << (unsigned long)status << std::dec << std::endl;
        CloseHandle(g_StopEvent);
        g_StopEvent = nullptr;
        return 1;
    }

    if (AkQuerySessionState(backend.Session, &session_state) != AK_STATUS_SUCCESS) {
        session_state.SessionId = 0ull;
    }

    std::wcout << L"control_session=" << session_state.SessionId << std::endl;
    std::wcout << L"queue_depth=" << config.queueDepth
               << L", slot_bytes=" << config.writeSlotBytes
               << L", sector_size=" << config.sectorSize
               << L", disk_bytes=" << config.diskSizeBytes
               << std::endl;
    std::wcout << L"state=ready(appkernel-host)" << std::endl;

    backend.EventThread = std::thread(RunEventLoop, &backend);

    RunCommandLoop(&backend);
    (void)WaitForSingleObject(g_StopEvent, INFINITE);

    (void)RemoveAllManagedDisks(&backend, true);
    backend.Stop.store(true, std::memory_order_relaxed);
    if (backend.EventThread.joinable()) {
        backend.EventThread.join();
    }

    (void)AkQuerySessionStats(backend.Session, &session_stats);
    PrintBackendStats(&backend);

    ok =
        (session_stats.CommandFailures == 0ull) &&
        (session_stats.ProtocolFailures == 0ull);

    AkClose(backend.Session);
    backend.Session = nullptr;

    (void)SetConsoleCtrlHandler(ConsoleCtrlHandler, FALSE);
    CloseHandle(g_StopEvent);
    g_StopEvent = nullptr;
    return ok ? 0 : 1;
}
