#define NOMINMAX

#include <Windows.h>
#include <SetupAPI.h>
#include <Ntddstor.h>
#include <WinIoCtl.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <cstdint>
#include <cstdlib>
#include <cwctype>
#include <deque>
#include <exception>
#include <iostream>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "yumedisk_proto.h"

namespace {

constexpr LONG kStatusSuccess = 0x00000000L;
constexpr LONG kStatusInvalidParameter = static_cast<LONG>(0xC000000DL);
constexpr LONG kStatusDeviceNotConnected = static_cast<LONG>(0xC000009DL);

constexpr ULONG kDefaultTargetId = 0;
constexpr ULONG kDefaultSectorSize = 4096;
constexpr size_t kDefaultQueueDepth = 32;
constexpr size_t kDefaultWriteSlotBytes = 64 * 1024;
constexpr uint64_t kDefaultDiskSizeBytes = 64ull * 1024ull * 1024ull;
constexpr size_t kMediumStripeBytes = 256 * 1024;
constexpr size_t kMaxAckBatchRanges = 128;
constexpr DWORD kWriteAckFlushDelayMs = 10;

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

struct ControlContext {
    HANDLE file = INVALID_HANDLE_VALUE;
    uint64_t sessionId = 0;
};

struct DiskIdentity {
    std::wstring path;
    std::wstring vendor;
    std::wstring product;
    uint64_t lengthBytes = 0;
    DWORD deviceNumber = std::numeric_limits<DWORD>::max();
};

struct BackendStats {
    std::atomic<uint64_t> readSlotPosts{0};
    std::atomic<uint64_t> readSlotCompletions{0};
    std::atomic<uint64_t> readAckCommands{0};
    std::atomic<uint64_t> writeSlotPosts{0};
    std::atomic<uint64_t> writeSlotCompletions{0};
    std::atomic<uint64_t> piggybackAckRanges{0};
    std::atomic<uint64_t> flushedAckRanges{0};
    std::atomic<uint64_t> flushedAckCommands{0};
    std::atomic<uint64_t> commandFailures{0};
    std::atomic<uint64_t> protocolFailures{0};
};

struct BackendContext;

struct ManagedDisk {
    BackendContext* backend = nullptr;
    ULONG targetId = 0;
    ULONG sectorSize = 0;
    uint64_t diskSizeBytes = 0;
    DiskIdentity identity{};
    std::vector<unsigned char> medium;
    std::vector<std::unique_ptr<std::shared_mutex>> stripeLocks;
    std::atomic<bool> stop{false};
    std::mutex ackLock;
    std::condition_variable ackCv;
    std::deque<YUMEDISK_WRITE_ACK_RANGE> pendingWriteAcks;
    std::vector<std::thread> readWorkers;
    std::vector<std::thread> writeWorkers;
    std::thread ackFlushThread;
};

struct BackendContext {
    ControlContext control;
    AppConfig config;
    std::atomic<bool> stop{false};
    BackendStats stats;
    std::mutex disksLock;
    std::map<ULONG, std::shared_ptr<ManagedDisk>> disks;
    std::mutex logLock;
};

HANDLE g_StopEvent = nullptr;

BOOL WINAPI ConsoleCtrlHandler(DWORD controlType) {
    switch (controlType) {
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

void LogLine(BackendContext* context, const std::wstring& text) {
    std::lock_guard<std::mutex> guard(context->logLock);
    std::wcerr << text << std::endl;
}

std::wstring Utf16FromAnsi(const char* text) {
    if (text == nullptr || *text == '\0') {
        return {};
    }

    const int length = MultiByteToWideChar(CP_ACP, 0, text, -1, nullptr, 0);
    if (length <= 1) {
        return {};
    }

    std::wstring result(static_cast<size_t>(length), L'\0');
    MultiByteToWideChar(CP_ACP, 0, text, -1, result.data(), length);
    if (!result.empty() && result.back() == L'\0') {
        result.pop_back();
    }
    return result;
}

std::wstring TrimDescriptor(const std::wstring& text) {
    size_t begin = 0;
    size_t end = text.size();

    while (begin < end && iswspace(text[begin]) != 0) {
        ++begin;
    }
    while (end > begin && iswspace(text[end - 1]) != 0) {
        --end;
    }

    return text.substr(begin, end - begin);
}

bool ContainsInsensitive(const std::wstring& haystack, const std::wstring& needle) {
    if (needle.empty() || haystack.size() < needle.size()) {
        return false;
    }

    for (size_t i = 0; i + needle.size() <= haystack.size(); ++i) {
        bool match = true;
        for (size_t j = 0; j < needle.size(); ++j) {
            if (towlower(haystack[i + j]) != towlower(needle[j])) {
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

std::vector<unsigned char> MakeMessageBuffer(ULONG command, ULONG payloadLength, ULONG capacityPayloadLength = 0) {
    if (capacityPayloadLength < payloadLength) {
        capacityPayloadLength = payloadLength;
    }

    std::vector<unsigned char> buffer(YUMEDISK_MESSAGE_BASE_SIZE + capacityPayloadLength, 0);
    auto* message = reinterpret_cast<PYUMEDISK_MESSAGE>(buffer.data());
    message->Header.Size = static_cast<ULONG>(buffer.size());
    message->Header.Version = YUMEDISK_PROTOCOL_VERSION;
    message->Header.Command = command;
    message->Header.PayloadLength = payloadLength;
    return buffer;
}

void InitMessageHeader(
    PYUMEDISK_MESSAGE message,
    ULONG bufferSize,
    ULONG command,
    ULONG payloadLength,
    uint64_t sessionId,
    ULONG targetId,
    ULONG flags = 0) {
    message->Header.Size = bufferSize;
    message->Header.Version = YUMEDISK_PROTOCOL_VERSION;
    message->Header.Command = command;
    message->Header.Status = 0;
    message->Header.SessionId = sessionId;
    message->Header.TxId = 0;
    message->Header.TargetId = targetId;
    message->Header.Flags = flags;
    message->Header.PayloadLength = payloadLength;
    message->Header.Reserved = 0;
}

bool SendIoControl(
    HANDLE file,
    void* inputBuffer,
    DWORD inputBufferSize,
    void* outputBuffer,
    DWORD outputBufferSize,
    DWORD* bytesReturned = nullptr,
    OVERLAPPED* reusedOverlapped = nullptr) {
    OVERLAPPED localOverlapped{};
    OVERLAPPED* overlapped;
    bool ownsEvent;
    DWORD transferred;

    ownsEvent = false;
    transferred = 0;

    if (reusedOverlapped == nullptr) {
        localOverlapped.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (localOverlapped.hEvent == nullptr) {
            return false;
        }

        overlapped = &localOverlapped;
        ownsEvent = true;
    } else {
        HANDLE eventHandle;
        OVERLAPPED resetOverlapped{};

        eventHandle = reusedOverlapped->hEvent;
        if (eventHandle == nullptr) {
            eventHandle = CreateEventW(nullptr, TRUE, FALSE, nullptr);
            if (eventHandle == nullptr) {
                return false;
            }

            ownsEvent = true;
        }

        resetOverlapped.hEvent = eventHandle;
        *reusedOverlapped = resetOverlapped;
        ResetEvent(eventHandle);
        overlapped = reusedOverlapped;
    }

    BOOL ok = DeviceIoControl(
        file,
        IOCTL_YUMEDISK_APP_COMMAND,
        inputBuffer,
        inputBufferSize,
        outputBuffer,
        outputBufferSize,
        &transferred,
        overlapped);

    if (!ok && GetLastError() == ERROR_IO_PENDING) {
        if (WaitForSingleObject(overlapped->hEvent, INFINITE) != WAIT_OBJECT_0) {
            if (ownsEvent) {
                CloseHandle(overlapped->hEvent);
            }
            return false;
        }

        ok = GetOverlappedResult(file, overlapped, &transferred, FALSE);
    }

    if (bytesReturned != nullptr) {
        *bytesReturned = transferred;
    }

    if (ownsEvent) {
        CloseHandle(overlapped->hEvent);
        if (reusedOverlapped != nullptr) {
            reusedOverlapped->hEvent = nullptr;
        }
    }

    return ok == TRUE;
}

bool SendCommand(HANDLE file, std::vector<unsigned char>& buffer, DWORD* bytesReturned = nullptr) {
    return SendIoControl(
        file,
        buffer.data(),
        static_cast<DWORD>(buffer.size()),
        buffer.data(),
        static_cast<DWORD>(buffer.size()),
        bytesReturned,
        nullptr);
}

std::vector<std::wstring> EnumerateDeviceInterfaces(const GUID* guid) {
    std::vector<std::wstring> paths;
    HDEVINFO info = SetupDiGetClassDevsW(guid, nullptr, nullptr, DIGCF_DEVICEINTERFACE | DIGCF_PRESENT);
    if (info == INVALID_HANDLE_VALUE) {
        return paths;
    }

    for (DWORD index = 0;; ++index) {
        SP_DEVICE_INTERFACE_DATA interfaceData{};
        interfaceData.cbSize = sizeof(interfaceData);
        if (!SetupDiEnumDeviceInterfaces(info, nullptr, guid, index, &interfaceData)) {
            if (GetLastError() == ERROR_NO_MORE_ITEMS) {
                break;
            }
            continue;
        }

        DWORD requiredSize = 0;
        SetupDiGetDeviceInterfaceDetailW(info, &interfaceData, nullptr, 0, &requiredSize, nullptr);
        if (requiredSize == 0) {
            continue;
        }

        std::vector<unsigned char> detailBuffer(requiredSize, 0);
        auto* detail = reinterpret_cast<PSP_DEVICE_INTERFACE_DETAIL_DATA_W>(detailBuffer.data());
        detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
        if (!SetupDiGetDeviceInterfaceDetailW(info, &interfaceData, detail, requiredSize, nullptr, nullptr)) {
            continue;
        }

        paths.emplace_back(detail->DevicePath);
    }

    SetupDiDestroyDeviceInfoList(info);
    return paths;
}

HANDLE OpenFirstDeviceInterface(const GUID* guid, bool overlapped) {
    const auto paths = EnumerateDeviceInterfaces(guid);
    DWORD lastError = ERROR_NOT_FOUND;
    for (const auto& path : paths) {
        const HANDLE file = CreateFileW(
            path.c_str(),
            GENERIC_READ | GENERIC_WRITE,
            FILE_SHARE_READ | FILE_SHARE_WRITE,
            nullptr,
            OPEN_EXISTING,
            overlapped ? FILE_FLAG_OVERLAPPED : FILE_ATTRIBUTE_NORMAL,
            nullptr);
        if (file != INVALID_HANDLE_VALUE) {
            return file;
        }

        lastError = GetLastError();
    }

    SetLastError(lastError);
    return INVALID_HANDLE_VALUE;
}

bool QuerySessionId(ControlContext* control) {
    auto buffer = MakeMessageBuffer(YumeDiskCommandQueryInfo, 0, sizeof(YUMEDISK_QUERY_INFO));
    auto* message = reinterpret_cast<PYUMEDISK_MESSAGE>(buffer.data());

    if (!SendCommand(control->file, buffer, nullptr)) {
        return false;
    }

    if (message->Header.Status != kStatusSuccess ||
        message->Header.PayloadLength < sizeof(YUMEDISK_QUERY_INFO) ||
        message->Header.SessionId == 0) {
        return false;
    }

    control->sessionId = message->Header.SessionId;
    return true;
}

bool SendHeartbeat(ControlContext* control) {
    auto buffer = MakeMessageBuffer(YumeDiskCommandHeartbeat, 0);
    auto* message = reinterpret_cast<PYUMEDISK_MESSAGE>(buffer.data());
    message->Header.SessionId = control->sessionId;

    if (!SendCommand(control->file, buffer, nullptr)) {
        return false;
    }

    return message->Header.Status == kStatusSuccess;
}

bool QueryInfo(ControlContext* control, YUMEDISK_QUERY_INFO* info) {
    auto buffer = MakeMessageBuffer(YumeDiskCommandQueryInfo, 0, sizeof(YUMEDISK_QUERY_INFO));
    auto* message = reinterpret_cast<PYUMEDISK_MESSAGE>(buffer.data());

    if (!SendCommand(control->file, buffer, nullptr)) {
        return false;
    }

    if (message->Header.Status != kStatusSuccess ||
        message->Header.PayloadLength < sizeof(YUMEDISK_QUERY_INFO)) {
        return false;
    }

    if (message->Header.SessionId != 0) {
        control->sessionId = message->Header.SessionId;
    }

    *info = *reinterpret_cast<PYUMEDISK_QUERY_INFO>(message->Payload);
    return true;
}

bool CreateDisk(ControlContext* control, const AppConfig& config, ULONG targetId) {
    auto buffer = MakeMessageBuffer(YumeDiskCommandCreateDisk, sizeof(YUMEDISK_CREATE_DISK));
    auto* message = reinterpret_cast<PYUMEDISK_MESSAGE>(buffer.data());
    auto* request = reinterpret_cast<PYUMEDISK_CREATE_DISK>(message->Payload);

    message->Header.SessionId = control->sessionId;
    request->TargetId = targetId;
    request->SectorSize = config.sectorSize;
    request->SectorCount = config.diskSizeBytes / config.sectorSize;
    request->DiskSizeBytes = config.diskSizeBytes;

    if (!SendCommand(control->file, buffer, nullptr)) {
        return false;
    }

    return message->Header.Status == kStatusSuccess;
}

bool RemoveDisk(ControlContext* control, ULONG targetId) {
    auto buffer = MakeMessageBuffer(YumeDiskCommandRemoveDisk, sizeof(YUMEDISK_REMOVE_DISK));
    auto* message = reinterpret_cast<PYUMEDISK_MESSAGE>(buffer.data());
    auto* request = reinterpret_cast<PYUMEDISK_REMOVE_DISK>(message->Payload);

    message->Header.SessionId = control->sessionId;
    request->TargetId = targetId;
    request->Flags = 0;

    if (!SendCommand(control->file, buffer, nullptr)) {
        return false;
    }

    return message->Header.Status == kStatusSuccess;
}

bool RemoveAllDisks(ControlContext* control, ULONG flags = 0) {
    auto buffer = MakeMessageBuffer(YumeDiskCommandRemoveAllDisks, 0);
    auto* message = reinterpret_cast<PYUMEDISK_MESSAGE>(buffer.data());
    message->Header.SessionId = control->sessionId;
    message->Header.Flags = flags;

    if (!SendCommand(control->file, buffer, nullptr)) {
        return false;
    }

    return message->Header.Status == kStatusSuccess;
}

bool QueryDiskIdentity(const std::wstring& path, DiskIdentity* identity) {
    identity->path = path;
    identity->vendor.clear();
    identity->product.clear();
    identity->lengthBytes = 0;
    identity->deviceNumber = std::numeric_limits<DWORD>::max();

    const HANDLE handle = CreateFileW(
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

    STORAGE_PROPERTY_QUERY query{};
    query.PropertyId = StorageDeviceProperty;
    query.QueryType = PropertyStandardQuery;

    STORAGE_DESCRIPTOR_HEADER header{};
    DWORD bytesReturned = 0;
    if (DeviceIoControl(
            handle,
            IOCTL_STORAGE_QUERY_PROPERTY,
            &query,
            sizeof(query),
            &header,
            sizeof(header),
            &bytesReturned,
            nullptr) &&
        header.Size >= sizeof(STORAGE_DEVICE_DESCRIPTOR)) {
        std::vector<unsigned char> descriptorBuffer(header.Size, 0);
        if (DeviceIoControl(
                handle,
                IOCTL_STORAGE_QUERY_PROPERTY,
                &query,
                sizeof(query),
                descriptorBuffer.data(),
                static_cast<DWORD>(descriptorBuffer.size()),
                &bytesReturned,
                nullptr)) {
            const auto* descriptor =
                reinterpret_cast<const STORAGE_DEVICE_DESCRIPTOR*>(descriptorBuffer.data());
            if (descriptor->VendorIdOffset != 0 && descriptor->VendorIdOffset < descriptorBuffer.size()) {
                identity->vendor = TrimDescriptor(Utf16FromAnsi(
                    reinterpret_cast<const char*>(descriptorBuffer.data() + descriptor->VendorIdOffset)));
            }
            if (descriptor->ProductIdOffset != 0 && descriptor->ProductIdOffset < descriptorBuffer.size()) {
                identity->product = TrimDescriptor(Utf16FromAnsi(
                    reinterpret_cast<const char*>(descriptorBuffer.data() + descriptor->ProductIdOffset)));
            }
        }
    }

    GET_LENGTH_INFORMATION lengthInfo{};
    if (DeviceIoControl(
            handle,
            IOCTL_DISK_GET_LENGTH_INFO,
            nullptr,
            0,
            &lengthInfo,
            sizeof(lengthInfo),
            &bytesReturned,
            nullptr)) {
        identity->lengthBytes = static_cast<uint64_t>(lengthInfo.Length.QuadPart);
    }

    STORAGE_DEVICE_NUMBER deviceNumber{};
    if (DeviceIoControl(
            handle,
            IOCTL_STORAGE_GET_DEVICE_NUMBER,
            nullptr,
            0,
            &deviceNumber,
            sizeof(deviceNumber),
            &bytesReturned,
            nullptr)) {
        identity->deviceNumber = deviceNumber.DeviceNumber;
    }

    CloseHandle(handle);
    return true;
}

bool IsTargetDiskCandidate(const DiskIdentity& identity, const AppConfig& config) {
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

std::vector<DiskIdentity> EnumerateVisibleYumeDisks(const AppConfig& config) {
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

size_t ComputeStripeCount(uint64_t diskSizeBytes) {
    const uint64_t stripeCount64 = (diskSizeBytes + kMediumStripeBytes - 1) / kMediumStripeBytes;
    if (stripeCount64 == 0) {
        return 1;
    }

    return static_cast<size_t>(stripeCount64);
}

std::vector<size_t> ComputeStripeIndices(size_t offset, size_t length) {
    std::vector<size_t> indices;

    if (length == 0) {
        return indices;
    }

    const size_t first = offset / kMediumStripeBytes;
    const size_t last = (offset + length - 1) / kMediumStripeBytes;
    indices.reserve(last - first + 1);
    for (size_t index = first; index <= last; ++index) {
        indices.push_back(index);
    }

    return indices;
}

std::vector<std::shared_lock<std::shared_mutex>> AcquireReadStripeLocks(
    ManagedDisk* disk,
    size_t offset,
    size_t length) {
    auto indices = ComputeStripeIndices(offset, length);
    std::vector<std::shared_lock<std::shared_mutex>> locks;

    locks.reserve(indices.size());
    for (const size_t index : indices) {
        locks.emplace_back(*disk->stripeLocks[index]);
    }

    return locks;
}

std::vector<std::unique_lock<std::shared_mutex>> AcquireWriteStripeLocks(
    ManagedDisk* disk,
    size_t offset,
    size_t length) {
    auto indices = ComputeStripeIndices(offset, length);
    std::vector<std::unique_lock<std::shared_mutex>> locks;

    locks.reserve(indices.size());
    for (const size_t index : indices) {
        locks.emplace_back(*disk->stripeLocks[index]);
    }

    return locks;
}

std::shared_ptr<ManagedDisk> FindManagedDisk(BackendContext* context, ULONG targetId) {
    std::lock_guard<std::mutex> guard(context->disksLock);
    const auto it = context->disks.find(targetId);
    if (it == context->disks.end()) {
        return nullptr;
    }

    return it->second;
}

std::vector<std::shared_ptr<ManagedDisk>> SnapshotManagedDisks(BackendContext* context) {
    std::vector<std::shared_ptr<ManagedDisk>> disks;

    std::lock_guard<std::mutex> guard(context->disksLock);
    disks.reserve(context->disks.size());
    for (const auto& entry : context->disks) {
        disks.push_back(entry.second);
    }

    return disks;
}

void InsertManagedDisk(BackendContext* context, const std::shared_ptr<ManagedDisk>& disk) {
    std::lock_guard<std::mutex> guard(context->disksLock);
    context->disks.emplace(disk->targetId, disk);
}

void RemoveManagedDiskFromMap(BackendContext* context, ULONG targetId) {
    std::lock_guard<std::mutex> guard(context->disksLock);
    context->disks.erase(targetId);
}

bool ManagedDiskExists(BackendContext* context, ULONG targetId) {
    std::lock_guard<std::mutex> guard(context->disksLock);
    return context->disks.find(targetId) != context->disks.end();
}

ULONG FindFirstFreeTarget(BackendContext* context) {
    std::lock_guard<std::mutex> guard(context->disksLock);
    for (ULONG targetId = YUMEDISK_MIN_TARGET_ID; targetId <= YUMEDISK_MAX_USABLE_TARGET_ID; ++targetId) {
        if (context->disks.find(targetId) == context->disks.end()) {
            return targetId;
        }
    }

    return YUMEDISK_MAX_TARGETS;
}

bool CanMergeAckRanges(const YUMEDISK_WRITE_ACK_RANGE& left, const YUMEDISK_WRITE_ACK_RANGE& right) {
    const uint64_t leftEnd = static_cast<uint64_t>(left.SeqBase) + left.SeqCount;
    return left.EventId == right.EventId &&
        left.IoStatus == right.IoStatus &&
        leftEnd == right.SeqBase;
}

void AppendWriteAckRangeLocked(ManagedDisk* disk, const YUMEDISK_WRITE_ACK_RANGE& range) {
    if (!disk->pendingWriteAcks.empty() && CanMergeAckRanges(disk->pendingWriteAcks.back(), range)) {
        disk->pendingWriteAcks.back().SeqCount += range.SeqCount;
        return;
    }

    disk->pendingWriteAcks.push_back(range);
}

void EnqueueWriteAckRange(ManagedDisk* disk, const YUMEDISK_WRITE_ACK_RANGE& range) {
    {
        std::lock_guard<std::mutex> guard(disk->ackLock);
        AppendWriteAckRangeLocked(disk, range);
    }

    disk->ackCv.notify_one();
}

std::vector<YUMEDISK_WRITE_ACK_RANGE> StealWriteAckRangesLocked(ManagedDisk* disk, size_t maxRanges) {
    std::vector<YUMEDISK_WRITE_ACK_RANGE> ranges;

    while (!disk->pendingWriteAcks.empty() && ranges.size() < maxRanges) {
        ranges.push_back(disk->pendingWriteAcks.front());
        disk->pendingWriteAcks.pop_front();
    }

    return ranges;
}

std::vector<YUMEDISK_WRITE_ACK_RANGE> StealWriteAckRanges(ManagedDisk* disk, size_t maxRanges) {
    std::lock_guard<std::mutex> guard(disk->ackLock);
    return StealWriteAckRangesLocked(disk, maxRanges);
}

void RequeueWriteAckRanges(ManagedDisk* disk, const std::vector<YUMEDISK_WRITE_ACK_RANGE>& ranges) {
    if (ranges.empty()) {
        return;
    }

    {
        std::lock_guard<std::mutex> guard(disk->ackLock);
        for (auto it = ranges.rbegin(); it != ranges.rend(); ++it) {
            disk->pendingWriteAcks.push_front(*it);
        }
    }

    disk->ackCv.notify_one();
}

size_t ComputeWriteAckBatchPayloadLength(size_t rangeCount) {
    if (rangeCount == 0) {
        return 0;
    }

    return static_cast<size_t>(YUMEDISK_WRITE_ACK_BATCH_SIZE(static_cast<UINT32>(rangeCount)));
}

void FillWriteAckBatchPayload(
    UCHAR* payload,
    const std::vector<YUMEDISK_WRITE_ACK_RANGE>& ranges) {
    auto* batch = reinterpret_cast<PYUMEDISK_WRITE_ACK_BATCH>(payload);

    batch->RangeCount = static_cast<UINT32>(ranges.size());
    batch->Reserved = 0;
    if (!ranges.empty()) {
        std::memcpy(batch->Ranges, ranges.data(), ranges.size() * sizeof(YUMEDISK_WRITE_ACK_RANGE));
    }
}

bool SendWriteAckBatch(
    BackendContext* context,
    ManagedDisk* disk,
    const std::vector<YUMEDISK_WRITE_ACK_RANGE>& ranges) {
    const size_t payloadLength = ComputeWriteAckBatchPayloadLength(ranges.size());
    auto buffer = MakeMessageBuffer(
        YumeDiskCommandWriteAckBatch,
        static_cast<ULONG>(payloadLength),
        static_cast<ULONG>(payloadLength));
    auto* message = reinterpret_cast<PYUMEDISK_MESSAGE>(buffer.data());

    InitMessageHeader(
        message,
        static_cast<ULONG>(buffer.size()),
        YumeDiskCommandWriteAckBatch,
        static_cast<ULONG>(payloadLength),
        context->control.sessionId,
        disk->targetId);
    if (payloadLength != 0) {
        FillWriteAckBatchPayload(message->Payload, ranges);
    }

    if (!SendCommand(context->control.file, buffer, nullptr)) {
        context->stats.commandFailures.fetch_add(1, std::memory_order_relaxed);
        return false;
    }

    if (message->Header.Status != kStatusSuccess) {
        context->stats.protocolFailures.fetch_add(1, std::memory_order_relaxed);
        return false;
    }

    return true;
}

void WriteAckFlushWorker(BackendContext* context, const std::shared_ptr<ManagedDisk>& disk) {
    for (;;) {
        std::vector<YUMEDISK_WRITE_ACK_RANGE> ranges;

        {
            std::unique_lock<std::mutex> lock(disk->ackLock);
            disk->ackCv.wait(lock, [&]() {
                return context->stop.load(std::memory_order_relaxed) ||
                    disk->stop.load(std::memory_order_relaxed) ||
                    !disk->pendingWriteAcks.empty();
            });

            if (context->stop.load(std::memory_order_relaxed) || disk->stop.load(std::memory_order_relaxed)) {
                break;
            }

            disk->ackCv.wait_for(lock, std::chrono::milliseconds(kWriteAckFlushDelayMs), [&]() {
                return context->stop.load(std::memory_order_relaxed) ||
                    disk->stop.load(std::memory_order_relaxed) ||
                    disk->pendingWriteAcks.empty();
            });

            if (context->stop.load(std::memory_order_relaxed) || disk->stop.load(std::memory_order_relaxed)) {
                break;
            }

            if (disk->pendingWriteAcks.empty()) {
                continue;
            }

            ranges = StealWriteAckRangesLocked(disk.get(), kMaxAckBatchRanges);
        }

        if (!SendWriteAckBatch(context, disk.get(), ranges)) {
            if (!context->stop.load(std::memory_order_relaxed) && !disk->stop.load(std::memory_order_relaxed)) {
                RequeueWriteAckRanges(disk.get(), ranges);
                LogLine(context, L"WRITE_ACK_BATCH flush failed");
            }
            continue;
        }

        context->stats.flushedAckCommands.fetch_add(1, std::memory_order_relaxed);
        context->stats.flushedAckRanges.fetch_add(ranges.size(), std::memory_order_relaxed);
    }
}

bool IsExpectedWorkerStop(BackendContext* context, ManagedDisk* disk) {
    return context->stop.load(std::memory_order_relaxed) || disk->stop.load(std::memory_order_relaxed);
}

bool ValidateReadSlotEvent(
    BackendContext* context,
    ManagedDisk* disk,
    const YUMEDISK_READ_SLOT_EVENT& event) {
    if (event.EventId == 0 ||
        event.TargetId != disk->targetId ||
        (event.DataLength != 0 && event.DataLength % disk->sectorSize != 0)) {
        context->stats.protocolFailures.fetch_add(1, std::memory_order_relaxed);
        LogLine(context, L"POST_READ_SLOT returned invalid event payload");
        return false;
    }

    return true;
}

LONG CopyReadData(
    ManagedDisk* disk,
    const YUMEDISK_READ_SLOT_EVENT& event,
    std::vector<unsigned char>* readBuffer,
    ULONG* dataLength) {
    const uint64_t offset64 = event.Lba * static_cast<uint64_t>(disk->sectorSize);

    *dataLength = event.DataLength;
    if (offset64 > disk->medium.size() ||
        static_cast<uint64_t>(event.DataLength) > disk->medium.size() - offset64) {
        *dataLength = 0;
        return kStatusInvalidParameter;
    }

    readBuffer->resize(event.DataLength);
    if (event.DataLength != 0) {
        const size_t offset = static_cast<size_t>(offset64);
        auto locks = AcquireReadStripeLocks(disk, offset, event.DataLength);
        std::memcpy(readBuffer->data(), disk->medium.data() + offset, event.DataLength);
    }

    return kStatusSuccess;
}

void ReadSlotWorker(BackendContext* context, const std::shared_ptr<ManagedDisk>& disk) {
    OVERLAPPED overlapped{};
    YUMEDISK_READ_SLOT_EVENT event{};
    std::vector<unsigned char> requestBuffer;
    std::vector<unsigned char> readAckMessage;
    std::vector<unsigned char> readDataBuffer;

    overlapped.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (overlapped.hEvent == nullptr) {
        context->stats.commandFailures.fetch_add(1, std::memory_order_relaxed);
        return;
    }

    requestBuffer = MakeMessageBuffer(YumeDiskCommandPostReadSlot, 0);
    readAckMessage = MakeMessageBuffer(YumeDiskCommandReadAck, sizeof(YUMEDISK_READ_ACK));

    while (!IsExpectedWorkerStop(context, disk.get())) {
        auto* requestMessage = reinterpret_cast<PYUMEDISK_MESSAGE>(requestBuffer.data());
        std::memset(&event, 0, sizeof(event));
        InitMessageHeader(
            requestMessage,
            static_cast<ULONG>(requestBuffer.size()),
            YumeDiskCommandPostReadSlot,
            0,
            context->control.sessionId,
            disk->targetId);

        context->stats.readSlotPosts.fetch_add(1, std::memory_order_relaxed);
        if (!SendIoControl(
                context->control.file,
                requestBuffer.data(),
                static_cast<DWORD>(requestBuffer.size()),
                &event,
                sizeof(event),
                nullptr,
                &overlapped)) {
            if (!IsExpectedWorkerStop(context, disk.get())) {
                context->stats.commandFailures.fetch_add(1, std::memory_order_relaxed);
                LogLine(context, L"POST_READ_SLOT failed");
            }
            break;
        }

        if (IsExpectedWorkerStop(context, disk.get())) {
            break;
        }

        if (!ValidateReadSlotEvent(context, disk.get(), event)) {
            continue;
        }

        ULONG dataLength = 0;
        const LONG ioStatus = CopyReadData(disk.get(), event, &readDataBuffer, &dataLength);
        auto* readAckRequest = reinterpret_cast<PYUMEDISK_MESSAGE>(readAckMessage.data());
        auto* readAck = reinterpret_cast<PYUMEDISK_READ_ACK>(readAckRequest->Payload);

        InitMessageHeader(
            readAckRequest,
            static_cast<ULONG>(readAckMessage.size()),
            YumeDiskCommandReadAck,
            sizeof(YUMEDISK_READ_ACK),
            context->control.sessionId,
            disk->targetId);
        readAck->EventId = event.EventId;
        readAck->IoStatus = ioStatus;
        readAck->DataLength = dataLength;
        readAck->KernelVa = 0;

        if (!SendIoControl(
                context->control.file,
                readAckMessage.data(),
                static_cast<DWORD>(readAckMessage.size()),
                dataLength == 0 ? nullptr : readDataBuffer.data(),
                dataLength,
                nullptr,
                &overlapped)) {
            if (!IsExpectedWorkerStop(context, disk.get())) {
                context->stats.commandFailures.fetch_add(1, std::memory_order_relaxed);
                LogLine(context, L"READ_ACK failed");
            }
            break;
        }

        context->stats.readSlotCompletions.fetch_add(1, std::memory_order_relaxed);
        context->stats.readAckCommands.fetch_add(1, std::memory_order_relaxed);
    }

    CloseHandle(overlapped.hEvent);
}

bool ValidateWriteSlotHeader(
    BackendContext* context,
    ManagedDisk* disk,
    const YUMEDISK_WRITE_SLOT_HEADER& header,
    size_t slotCapacity) {
    if (header.EventId == 0 ||
        header.TargetId != disk->targetId ||
        header.TotalSeq == 0 ||
        header.Seq >= header.TotalSeq ||
        header.DataLength > slotCapacity - YUMEDISK_WRITE_SLOT_HEADER_BASE_SIZE ||
        (header.DataLength != 0 && header.DataLength % disk->sectorSize != 0)) {
        context->stats.protocolFailures.fetch_add(1, std::memory_order_relaxed);
        LogLine(context, L"POST_WRITE_SLOT returned invalid slot payload");
        return false;
    }

    return true;
}

LONG ApplyWriteSlotToMedium(ManagedDisk* disk, const YUMEDISK_WRITE_SLOT_HEADER& header) {
    const uint64_t offset64 = header.Lba * static_cast<uint64_t>(disk->sectorSize);

    if (offset64 > disk->medium.size() ||
        static_cast<uint64_t>(header.DataLength) > disk->medium.size() - offset64) {
        return kStatusInvalidParameter;
    }

    if (header.DataLength != 0) {
        const size_t offset = static_cast<size_t>(offset64);
        auto locks = AcquireWriteStripeLocks(disk, offset, header.DataLength);
        std::memcpy(disk->medium.data() + offset, header.Data, header.DataLength);
    }

    return kStatusSuccess;
}

void WriteSlotWorker(BackendContext* context, const std::shared_ptr<ManagedDisk>& disk) {
    OVERLAPPED overlapped{};
    const size_t slotCapacity = YUMEDISK_WRITE_SLOT_HEADER_BASE_SIZE + context->config.writeSlotBytes;
    std::vector<unsigned char> requestBuffer;
    std::vector<unsigned char> slotBuffer(slotCapacity, 0);

    overlapped.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (overlapped.hEvent == nullptr) {
        context->stats.commandFailures.fetch_add(1, std::memory_order_relaxed);
        return;
    }

    while (!IsExpectedWorkerStop(context, disk.get())) {
        const auto ackRanges = StealWriteAckRanges(disk.get(), kMaxAckBatchRanges);
        const size_t payloadLength = ComputeWriteAckBatchPayloadLength(ackRanges.size());

        requestBuffer = MakeMessageBuffer(
            YumeDiskCommandPostWriteSlot,
            static_cast<ULONG>(payloadLength),
            static_cast<ULONG>(payloadLength));
        auto* requestMessage = reinterpret_cast<PYUMEDISK_MESSAGE>(requestBuffer.data());
        std::memset(slotBuffer.data(), 0, slotBuffer.size());

        InitMessageHeader(
            requestMessage,
            static_cast<ULONG>(requestBuffer.size()),
            YumeDiskCommandPostWriteSlot,
            static_cast<ULONG>(payloadLength),
            context->control.sessionId,
            disk->targetId);
        if (payloadLength != 0) {
            FillWriteAckBatchPayload(requestMessage->Payload, ackRanges);
        }

        context->stats.writeSlotPosts.fetch_add(1, std::memory_order_relaxed);
        if (!SendIoControl(
                context->control.file,
                requestBuffer.data(),
                static_cast<DWORD>(requestBuffer.size()),
                slotBuffer.data(),
                static_cast<DWORD>(slotBuffer.size()),
                nullptr,
                &overlapped)) {
            if (!ackRanges.empty()) {
                RequeueWriteAckRanges(disk.get(), ackRanges);
            }

            if (!IsExpectedWorkerStop(context, disk.get())) {
                context->stats.commandFailures.fetch_add(1, std::memory_order_relaxed);
                LogLine(context, L"POST_WRITE_SLOT failed");
            }
            break;
        }

        if (!ackRanges.empty()) {
            context->stats.piggybackAckRanges.fetch_add(ackRanges.size(), std::memory_order_relaxed);
        }

        if (IsExpectedWorkerStop(context, disk.get())) {
            break;
        }

        const auto* slotHeader = reinterpret_cast<const YUMEDISK_WRITE_SLOT_HEADER*>(slotBuffer.data());
        if (!ValidateWriteSlotHeader(context, disk.get(), *slotHeader, slotCapacity)) {
            continue;
        }

        const LONG ioStatus = ApplyWriteSlotToMedium(disk.get(), *slotHeader);
        YUMEDISK_WRITE_ACK_RANGE range{};
        range.EventId = slotHeader->EventId;
        range.SeqBase = slotHeader->Seq;
        range.SeqCount = 1;
        range.IoStatus = ioStatus;
        range.Reserved = 0;
        EnqueueWriteAckRange(disk.get(), range);

        context->stats.writeSlotCompletions.fetch_add(1, std::memory_order_relaxed);
    }

    CloseHandle(overlapped.hEvent);
}

void SignalManagedDiskStop(ManagedDisk* disk, bool stop) {
    disk->stop.store(stop, std::memory_order_relaxed);
    disk->ackCv.notify_all();
}

void JoinManagedDiskWorkers(ManagedDisk* disk) {
    SignalManagedDiskStop(disk, true);

    if (disk->ackFlushThread.joinable()) {
        disk->ackFlushThread.join();
    }

    for (auto& worker : disk->readWorkers) {
        if (worker.joinable()) {
            worker.join();
        }
    }
    disk->readWorkers.clear();

    for (auto& worker : disk->writeWorkers) {
        if (worker.joinable()) {
            worker.join();
        }
    }
    disk->writeWorkers.clear();

    {
        std::lock_guard<std::mutex> guard(disk->ackLock);
        disk->pendingWriteAcks.clear();
    }
}

bool StartManagedDiskWorkers(BackendContext* context, const std::shared_ptr<ManagedDisk>& disk) {
    try {
        disk->ackFlushThread = std::thread(WriteAckFlushWorker, context, disk);
        disk->readWorkers.reserve(context->config.queueDepth);
        disk->writeWorkers.reserve(context->config.queueDepth);

        for (size_t index = 0; index < context->config.queueDepth; ++index) {
            disk->readWorkers.emplace_back(ReadSlotWorker, context, disk);
        }
        for (size_t index = 0; index < context->config.queueDepth; ++index) {
            disk->writeWorkers.emplace_back(WriteSlotWorker, context, disk);
        }
    } catch (const std::exception&) {
        JoinManagedDiskWorkers(disk.get());
        return false;
    }

    return true;
}

void PrintBackendStats(const BackendContext* context) {
    std::wcout << L"backend_read_slot_posts=" << context->stats.readSlotPosts.load(std::memory_order_relaxed)
               << L", backend_read_slot_completions=" << context->stats.readSlotCompletions.load(std::memory_order_relaxed)
               << L", backend_read_ack_commands=" << context->stats.readAckCommands.load(std::memory_order_relaxed)
               << L", backend_write_slot_posts=" << context->stats.writeSlotPosts.load(std::memory_order_relaxed)
               << L", backend_write_slot_completions=" << context->stats.writeSlotCompletions.load(std::memory_order_relaxed)
               << L", backend_piggyback_ack_ranges=" << context->stats.piggybackAckRanges.load(std::memory_order_relaxed)
               << L", backend_flushed_ack_commands=" << context->stats.flushedAckCommands.load(std::memory_order_relaxed)
               << L", backend_flushed_ack_ranges=" << context->stats.flushedAckRanges.load(std::memory_order_relaxed)
               << L", backend_command_failures=" << context->stats.commandFailures.load(std::memory_order_relaxed)
               << L", backend_protocol_failures=" << context->stats.protocolFailures.load(std::memory_order_relaxed)
               << std::endl;
}

void PrintRuntimeHelp() {
    std::cout
        << "commands:\n"
        << "  help           show commands\n"
        << "  query          query control protocol info\n"
        << "  ct [target]    create one disk on target and start slot workers\n"
        << "  rm <target>    remove one disk and stop its slot workers\n"
        << "  rm all         remove all disks and stop all slot workers\n"
        << "  ls             list managed and visible disks\n"
        << "  stats          print backend counters\n"
        << "  exit           close session and quit\n"
        << "\n"
        << "runtime:\n"
        << "  heartbeat stays in the background while the app is running\n"
        << "  each created disk gets read slot workers, write slot workers, and an idle ACK flush worker\n";
}

void PrintUsage() {
    std::cout
        << "RWTestApp [--queue-depth N] [--slot-bytes BYTES] [--io-size BYTES]\n"
        << "          [--disk-size-mb MB] [--sector-size BYTES] [--target ID]\n"
        << "\n"
        << "defaults:\n"
        << "  queue-depth = " << kDefaultQueueDepth << "\n"
        << "  slot-bytes  = " << kDefaultWriteSlotBytes << "\n"
        << "  disk-size-mb = " << (kDefaultDiskSizeBytes / (1024ull * 1024ull)) << "\n"
        << "  sector-size = " << kDefaultSectorSize << "\n"
        << "  target      = " << kDefaultTargetId << "\n"
        << "\n"
        << "behavior:\n"
        << "  create a disk with `ct`, then keep the app running while the benchmark hits the exposed PhysicalDrive\n";
}

bool ParseUnsigned(const char* text, uint64_t* value) {
    char* end = nullptr;
    const auto parsed = _strtoui64(text, &end, 0);
    if (end == text || *end != '\0') {
        return false;
    }

    *value = parsed;
    return true;
}

ParseResult ParseArgs(int argc, char** argv, AppConfig* config) {
    for (int index = 1; index < argc; ++index) {
        const std::string arg = argv[index];
        auto nextValue = [&](uint64_t* out) -> bool {
            if (index + 1 >= argc) {
                return false;
            }
            return ParseUnsigned(argv[++index], out);
        };

        uint64_t value = 0;
        if (arg == "--help" || arg == "-h") {
            return ParseResult::Help;
        }
        if (arg == "--queue-depth") {
            if (!nextValue(&value) || value == 0 || value > std::numeric_limits<size_t>::max()) {
                return ParseResult::Error;
            }
            config->queueDepth = static_cast<size_t>(value);
            continue;
        }
        if (arg == "--slot-bytes" || arg == "--io-size") {
            if (!nextValue(&value) || value == 0 || value > std::numeric_limits<size_t>::max()) {
                return ParseResult::Error;
            }
            config->writeSlotBytes = static_cast<size_t>(value);
            continue;
        }
        if (arg == "--disk-size-mb") {
            if (!nextValue(&value) || value == 0) {
                return ParseResult::Error;
            }
            config->diskSizeBytes = value * 1024ull * 1024ull;
            continue;
        }
        if (arg == "--sector-size") {
            if (!nextValue(&value) || value == 0 || value > std::numeric_limits<ULONG>::max()) {
                return ParseResult::Error;
            }
            config->sectorSize = static_cast<ULONG>(value);
            continue;
        }
        if (arg == "--target") {
            if (!nextValue(&value) || value > YUMEDISK_MAX_USABLE_TARGET_ID) {
                return ParseResult::Error;
            }
            config->targetId = static_cast<ULONG>(value);
            continue;
        }

        return ParseResult::Error;
    }

    if (config->writeSlotBytes < config->sectorSize ||
        config->writeSlotBytes % config->sectorSize != 0 ||
        config->diskSizeBytes % config->sectorSize != 0) {
        return ParseResult::Error;
    }

    return ParseResult::Ok;
}

bool ParseTargetToken(const std::string& token, ULONG* targetId) {
    uint64_t value = 0;
    if (!ParseUnsigned(token.c_str(), &value) || value > YUMEDISK_MAX_USABLE_TARGET_ID) {
        return false;
    }

    *targetId = static_cast<ULONG>(value);
    return true;
}

void ListManagedDisks(BackendContext* context) {
    const auto managedDisks = SnapshotManagedDisks(context);
    if (managedDisks.empty()) {
        std::wcout << L"disk_count=0" << std::endl;
    } else {
        std::wcout << L"disk_count=" << managedDisks.size() << std::endl;
        for (const auto& disk : managedDisks) {
            std::wcout << L"target=" << disk->targetId
                       << L", path=" << (disk->identity.path.empty() ? L"<pending-enumeration>" : disk->identity.path)
                       << L", read_workers=" << disk->readWorkers.size()
                       << L", write_workers=" << disk->writeWorkers.size()
                       << std::endl;
        }
    }

    const auto visibleDisks = EnumerateVisibleYumeDisks(context->config);
    std::wcout << L"visible_disk_count=" << visibleDisks.size() << std::endl;
    for (size_t i = 0; i < visibleDisks.size(); ++i) {
        const auto& disk = visibleDisks[i];
        std::wcout << L"visible_disk[" << i << L"]"
                   << L", path=" << disk.path
                   << L", device_number=" << disk.deviceNumber
                   << L", disk_bytes=" << disk.lengthBytes
                   << std::endl;
    }
}

bool CreateManagedDisk(BackendContext* context, ULONG targetId) {
    std::shared_ptr<ManagedDisk> disk;

    if (ManagedDiskExists(context, targetId)) {
        std::wcerr << L"create failed, target already exists: " << targetId << std::endl;
        return false;
    }

    if (!CreateDisk(&context->control, context->config, targetId)) {
        std::wcerr << L"create failed, target=" << targetId << std::endl;
        return false;
    }

    try {
        disk = std::make_shared<ManagedDisk>();
        disk->backend = context;
        disk->targetId = targetId;
        disk->sectorSize = context->config.sectorSize;
        disk->diskSizeBytes = context->config.diskSizeBytes;
        disk->medium.resize(static_cast<size_t>(context->config.diskSizeBytes), 0);
        const size_t stripeCount = ComputeStripeCount(context->config.diskSizeBytes);
        disk->stripeLocks.reserve(stripeCount);
        for (size_t index = 0; index < stripeCount; ++index) {
            disk->stripeLocks.push_back(std::make_unique<std::shared_mutex>());
        }
    } catch (const std::exception&) {
        (void)RemoveDisk(&context->control, targetId);
        std::wcerr << L"create failed, target=" << targetId << L", reason=memory-allocation" << std::endl;
        return false;
    }

    if (!StartManagedDiskWorkers(context, disk)) {
        SignalManagedDiskStop(disk.get(), true);
        (void)RemoveDisk(&context->control, targetId);
        JoinManagedDiskWorkers(disk.get());
        std::wcerr << L"create failed, target=" << targetId << L", reason=worker-start" << std::endl;
        return false;
    }

    InsertManagedDisk(context, disk);
    std::wcout << L"created target=" << targetId
               << L", queue_depth=" << context->config.queueDepth
               << L", slot_bytes=" << context->config.writeSlotBytes
               << std::endl;
    return true;
}

bool RemoveManagedDisk(BackendContext* context, ULONG targetId) {
    const auto disk = FindManagedDisk(context, targetId);
    if (disk == nullptr) {
        std::wcerr << L"remove failed, target not found: " << targetId << std::endl;
        return false;
    }

    SignalManagedDiskStop(disk.get(), true);
    if (!RemoveDisk(&context->control, targetId)) {
        SignalManagedDiskStop(disk.get(), false);
        std::wcerr << L"remove failed, target=" << targetId << std::endl;
        return false;
    }

    JoinManagedDiskWorkers(disk.get());
    RemoveManagedDiskFromMap(context, targetId);
    std::wcout << L"removed target=" << targetId << std::endl;
    return true;
}

bool RemoveAllManagedDisks(BackendContext* context, bool closeSession = false) {
    const auto disks = SnapshotManagedDisks(context);

    for (const auto& disk : disks) {
        SignalManagedDiskStop(disk.get(), true);
    }

    const ULONG flags = closeSession ? YUMEDISK_SESSION_CLOSE_FLAG : 0;
    if (!RemoveAllDisks(&context->control, flags)) {
        if (!closeSession) {
            for (const auto& disk : disks) {
                SignalManagedDiskStop(disk.get(), false);
            }

            std::wcerr << L"remove all failed" << std::endl;
            return false;
        }

        CancelIoEx(context->control.file, nullptr);
    }

    for (const auto& disk : disks) {
        JoinManagedDiskWorkers(disk.get());
    }

    {
        std::lock_guard<std::mutex> guard(context->disksLock);
        context->disks.clear();
    }

    std::wcout << L"removed_all=true" << std::endl;
    return true;
}

void RunQuery(BackendContext* context) {
    YUMEDISK_QUERY_INFO info{};
    if (!QueryInfo(&context->control, &info)) {
        std::wcerr << L"query failed" << std::endl;
        return;
    }

    std::wcout << L"protocol=" << info.ProtocolVersion
               << L", max_targets=" << info.MaxTargets
               << L", features=0x" << std::hex << info.Features << std::dec
               << L", session=" << context->control.sessionId
               << std::endl;
}

void RunCommandLoop(BackendContext* context) {
    PrintRuntimeHelp();
    while (!context->stop.load(std::memory_order_relaxed)) {
        std::cout << "> " << std::flush;

        std::string line;
        if (!std::getline(std::cin, line)) {
            SetEvent(g_StopEvent);
            break;
        }

        std::istringstream input(line);
        std::string command;
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
        if (command == "exit" || command == "quit") {
            SetEvent(g_StopEvent);
            break;
        }
        if (command == "ct") {
            std::string arg;
            ULONG targetId = context->config.targetId;
            if (input >> arg) {
                if (!ParseTargetToken(arg, &targetId)) {
                    std::wcerr << L"invalid target: " << std::wstring(arg.begin(), arg.end()) << std::endl;
                    continue;
                }
            } else {
                targetId = FindFirstFreeTarget(context);
                if (targetId >= YUMEDISK_MAX_TARGETS) {
                    std::wcerr << L"no free target" << std::endl;
                    continue;
                }
            }

            CreateManagedDisk(context, targetId);
            continue;
        }
        if (command == "rm") {
            std::string arg;
            if (!(input >> arg)) {
                std::wcerr << L"rm requires <target>|all" << std::endl;
                continue;
            }
            if (arg == "all") {
                RemoveAllManagedDisks(context);
                continue;
            }

            ULONG targetId = 0;
            if (!ParseTargetToken(arg, &targetId)) {
                std::wcerr << L"invalid target: " << std::wstring(arg.begin(), arg.end()) << std::endl;
                continue;
            }

            RemoveManagedDisk(context, targetId);
            continue;
        }

        std::wcerr << L"unknown command: " << std::wstring(command.begin(), command.end()) << std::endl;
    }
}

} // namespace

int main(int argc, char** argv) {
    AppConfig config;
    const ParseResult parseResult = ParseArgs(argc, argv, &config);
    if (parseResult == ParseResult::Help) {
        PrintUsage();
        return 0;
    }
    if (parseResult != ParseResult::Ok) {
        PrintUsage();
        return 1;
    }

    BackendContext backend{};
    backend.config = config;
    g_StopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (g_StopEvent == nullptr) {
        std::cerr << "create stop event failed, error=" << GetLastError() << std::endl;
        return 1;
    }
    SetConsoleCtrlHandler(ConsoleCtrlHandler, TRUE);

    backend.control.file = OpenFirstDeviceInterface(&GUID_YUMEDISK_CONTROL, true);
    if (backend.control.file == INVALID_HANDLE_VALUE) {
        const DWORD error = GetLastError();
        std::cerr << "open control device failed, error=" << error << std::endl;
        CloseHandle(g_StopEvent);
        g_StopEvent = nullptr;
        return 1;
    }

    if (!QuerySessionId(&backend.control)) {
        const DWORD error = GetLastError();
        std::cerr << "query session failed, error=" << error << std::endl;
        CloseHandle(backend.control.file);
        CloseHandle(g_StopEvent);
        g_StopEvent = nullptr;
        return 1;
    }

    std::wcout << L"control_session=" << backend.control.sessionId << std::endl;
    std::wcout << L"queue_depth=" << config.queueDepth
               << L", slot_bytes=" << config.writeSlotBytes
               << L", sector_size=" << config.sectorSize
               << L", disk_bytes=" << config.diskSizeBytes
               << std::endl;
    std::wcout << L"state=ready(slot-backend)" << std::endl;

    std::thread heartbeatThread([&backend]() {
        while (!backend.stop.load(std::memory_order_relaxed)) {
            const DWORD waitStatus = WaitForSingleObject(g_StopEvent, 1000);
            if (waitStatus != WAIT_TIMEOUT) {
                break;
            }

            if (!SendHeartbeat(&backend.control)) {
                backend.stop.store(true, std::memory_order_relaxed);
                SetEvent(g_StopEvent);
                break;
            }
        }
    });

    RunCommandLoop(&backend);
    WaitForSingleObject(g_StopEvent, INFINITE);
    backend.stop.store(true, std::memory_order_relaxed);
    const bool sessionClosed = RemoveAllManagedDisks(&backend, true);
    heartbeatThread.join();

    PrintBackendStats(&backend);

    if (!sessionClosed) {
        CancelIoEx(backend.control.file, nullptr);
        CloseHandle(backend.control.file);
        backend.control.file = INVALID_HANDLE_VALUE;
    }

    if (backend.control.file != INVALID_HANDLE_VALUE) {
        CloseHandle(backend.control.file);
        backend.control.file = INVALID_HANDLE_VALUE;
    }

    SetConsoleCtrlHandler(ConsoleCtrlHandler, FALSE);
    CloseHandle(g_StopEvent);
    g_StopEvent = nullptr;

    const bool ok =
        backend.stats.commandFailures.load(std::memory_order_relaxed) == 0 &&
        backend.stats.protocolFailures.load(std::memory_order_relaxed) == 0;
    return ok ? 0 : 1;
}
