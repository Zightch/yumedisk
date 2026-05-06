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
#include <deque>
#include <exception>
#include <iostream>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "yumedisk_proto.h"

namespace {

constexpr LONG kStatusSuccess = 0x00000000L;
constexpr LONG kStatusNotFound = static_cast<LONG>(0xC0000225L);
constexpr LONG kStatusInvalidParameter = static_cast<LONG>(0xC000000DL);

constexpr ULONG kDefaultTargetId = 0;
constexpr ULONG kDefaultSectorSize = 4096;
constexpr size_t kDefaultQueueDepth = 32;
constexpr size_t kMaxSlotEngineQueueDepth = MAXIMUM_WAIT_OBJECTS / 2;
constexpr size_t kDefaultWriteSlotBytes = 1024 * 1024;
constexpr uint64_t kDefaultDiskSizeBytes = 64ull * 1024ull * 1024ull;
constexpr size_t kMaxAckBatchRanges = 128;
constexpr DWORD kWriteAckFlushDelayMs = 10;
constexpr DWORD kRecoverableErrorRetryDelayMs = 10;
constexpr DWORD kSlotEnginePollMs = 10;
constexpr DWORD kDiskArrivalPollMs = 100;
constexpr DWORD kDiskArrivalTimeoutMs = 2000;

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
    std::atomic<uint64_t> flushedAckCommands{0};
    std::atomic<uint64_t> flushedAckRanges{0};
    std::atomic<uint64_t> writeAckRangeFailures{0};
    std::atomic<uint64_t> commandFailures{0};
    std::atomic<uint64_t> protocolFailures{0};
};

struct ReadSlotContext {
    OVERLAPPED overlapped{};
    std::vector<unsigned char> requestBuffer;
    std::vector<unsigned char> readAckBuffer;
    std::vector<unsigned char> readDataBuffer;
    YUMEDISK_READ_SLOT_EVENT event{};
    uint64_t slotId = 0;
    bool active = false;
    std::chrono::steady_clock::time_point retryAfter = std::chrono::steady_clock::time_point::min();
};

struct WriteSlotContext {
    OVERLAPPED overlapped{};
    std::vector<unsigned char> requestBuffer;
    std::vector<unsigned char> slotBuffer;
    uint64_t slotId = 0;
    bool active = false;
    std::chrono::steady_clock::time_point retryAfter = std::chrono::steady_clock::time_point::min();
};

struct BackendContext;

struct ManagedDisk {
    BackendContext* backend = nullptr;
    ULONG targetId = 0;
    ULONG sectorSize = 0;
    uint64_t diskSizeBytes = 0;
    DiskIdentity identity{};
    std::vector<unsigned char> medium;
    std::atomic<bool> stop{false};
    std::thread slotEngineThread;
    size_t workerCount = 0;
    std::mutex ackLock;
    std::deque<YUMEDISK_WRITE_ACK_RANGE> pendingWriteAcks;
    std::atomic<uint32_t> pendingWriteAckCount{0};
    std::atomic<uint32_t> activeReadSlots{0};
    std::atomic<uint32_t> activeWriteSlots{0};
};

struct BackendContext {
    ControlContext control;
    AppConfig config;
    std::atomic<bool> stop{false};
    std::atomic<uint64_t> nextTxId{1};
    BackendStats stats;
    std::mutex disksLock;
    std::map<ULONG, std::shared_ptr<ManagedDisk>> disks;
    std::mutex logLock;
};

using SteadyClock = std::chrono::steady_clock;

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
    ULONG flags = 0
) {
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

bool EnsureOverlappedEvent(OVERLAPPED* overlapped) {
    if (overlapped == nullptr) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return false;
    }

    if (overlapped->hEvent != nullptr) {
        return true;
    }

    overlapped->hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    return overlapped->hEvent != nullptr;
}

void CloseOverlappedEvent(OVERLAPPED* overlapped) {
    if (overlapped != nullptr && overlapped->hEvent != nullptr) {
        CloseHandle(overlapped->hEvent);
        overlapped->hEvent = nullptr;
    }
}

bool BeginAsyncIoControl(
    HANDLE file,
    void* inputBuffer,
    DWORD inputBufferSize,
    void* outputBuffer,
    DWORD outputBufferSize,
    OVERLAPPED* overlapped,
    DWORD* lastError = nullptr,
    DWORD* immediateBytes = nullptr
) {
    HANDLE eventHandle;
    OVERLAPPED resetOverlapped{};
    DWORD transferred = 0;
    DWORD error = ERROR_SUCCESS;
    BOOL ok;

    if (!EnsureOverlappedEvent(overlapped)) {
        if (lastError != nullptr) {
            *lastError = GetLastError();
        }
        return false;
    }

    eventHandle = overlapped->hEvent;
    resetOverlapped.hEvent = eventHandle;
    *overlapped = resetOverlapped;
    ResetEvent(eventHandle);

    ok = DeviceIoControl(
        file,
        IOCTL_YUMEDISK_APP_COMMAND,
        inputBuffer,
        inputBufferSize,
        outputBuffer,
        outputBufferSize,
        &transferred,
        overlapped);
    if (ok) {
        SetEvent(eventHandle);
        if (lastError != nullptr) {
            *lastError = ERROR_SUCCESS;
        }
        if (immediateBytes != nullptr) {
            *immediateBytes = transferred;
        }
        return true;
    }

    error = GetLastError();
    if (lastError != nullptr) {
        *lastError = error;
    }

    return error == ERROR_IO_PENDING;
}

bool FinishAsyncIoControl(
    HANDLE file,
    OVERLAPPED* overlapped,
    DWORD* bytesReturned = nullptr,
    DWORD* lastError = nullptr
) {
    DWORD transferred = 0;
    const BOOL ok = GetOverlappedResult(file, overlapped, &transferred, FALSE);

    if (bytesReturned != nullptr) {
        *bytesReturned = transferred;
    }

    if (ok) {
        if (lastError != nullptr) {
            *lastError = ERROR_SUCCESS;
        }
        return true;
    }

    if (lastError != nullptr) {
        *lastError = GetLastError();
    }
    return false;
}

bool SendIoControl(
    HANDLE file,
    void* inputBuffer,
    DWORD inputBufferSize,
    void* outputBuffer,
    DWORD outputBufferSize,
    DWORD* bytesReturned = nullptr,
    OVERLAPPED* reusedOverlapped = nullptr
) {
    OVERLAPPED localOverlapped{};
    OVERLAPPED* overlapped;
    bool ownsEvent;
    DWORD transferred = 0;

    ownsEvent = false;
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

HANDLE OpenFirstDeviceInterface(const GUID* guid) {
    const auto paths = EnumerateDeviceInterfaces(guid);
    DWORD lastError = ERROR_NOT_FOUND;

    for (const auto& path : paths) {
        const HANDLE file = CreateFileW(
            path.c_str(),
            GENERIC_READ | GENERIC_WRITE,
            FILE_SHARE_READ | FILE_SHARE_WRITE,
            nullptr,
            OPEN_EXISTING,
            FILE_FLAG_OVERLAPPED,
            nullptr);
        if (file != INVALID_HANDLE_VALUE) {
            return file;
        }

        lastError = GetLastError();
    }

    SetLastError(lastError);
    return INVALID_HANDLE_VALUE;
}

uint64_t AllocateTxId(BackendContext* context) {
    uint64_t value = context->nextTxId.fetch_add(1, std::memory_order_relaxed);
    if (value == 0) {
        value = context->nextTxId.fetch_add(1, std::memory_order_relaxed);
    }
    return value;
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

bool QueryDebugState(ControlContext* control, YUMEDISK_DEBUG_STATE* debugState) {
    auto buffer = MakeMessageBuffer(YumeDiskCommandQueryDebugState, 0, sizeof(YUMEDISK_DEBUG_STATE));
    auto* message = reinterpret_cast<PYUMEDISK_MESSAGE>(buffer.data());
    message->Header.SessionId = control->sessionId;

    if (!SendCommand(control->file, buffer, nullptr)) {
        return false;
    }

    if (message->Header.Status != kStatusSuccess ||
        message->Header.PayloadLength < sizeof(YUMEDISK_DEBUG_STATE)) {
        return false;
    }

    *debugState = *reinterpret_cast<PYUMEDISK_DEBUG_STATE>(message->Payload);
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

bool CancelSlot(ControlContext* control, ULONG targetId, UINT32 slotType, uint64_t slotId) {
    auto buffer = MakeMessageBuffer(YumeDiskCommandCancelSlot, sizeof(YUMEDISK_CANCEL_SLOT));
    auto* message = reinterpret_cast<PYUMEDISK_MESSAGE>(buffer.data());
    auto* request = reinterpret_cast<PYUMEDISK_CANCEL_SLOT>(message->Payload);

    message->Header.SessionId = control->sessionId;
    message->Header.TargetId = targetId;
    request->SlotId = slotId;
    request->SlotType = slotType;
    request->TargetId = targetId;

    if (!SendCommand(control->file, buffer, nullptr)) {
        return false;
    }

    return message->Header.Status == kStatusSuccess || message->Header.Status == kStatusNotFound;
}

bool QueryDiskIdentity(const std::wstring& path, DiskIdentity* identity) {
    HANDLE handle;
    STORAGE_PROPERTY_QUERY query{};
    STORAGE_DESCRIPTOR_HEADER header{};
    DWORD bytesReturned;

    identity->path = path;
    identity->vendor.clear();
    identity->product.clear();
    identity->lengthBytes = 0;
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
    bytesReturned = 0;

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

std::wstring MakePhysicalDrivePath(DWORD deviceNumber) {
    if (deviceNumber == std::numeric_limits<DWORD>::max()) {
        return {};
    }

    return LR"(\\.\PhysicalDrive)" + std::to_wstring(deviceNumber);
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

std::shared_ptr<ManagedDisk> FindManagedDisk(BackendContext* context, ULONG targetId) {
    std::lock_guard<std::mutex> guard(context->disksLock);
    const auto it = context->disks.find(targetId);
    if (it == context->disks.end()) {
        return nullptr;
    }

    return it->second;
}

void InsertManagedDisk(BackendContext* context, const std::shared_ptr<ManagedDisk>& disk) {
    std::lock_guard<std::mutex> guard(context->disksLock);
    context->disks[disk->targetId] = disk;
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

std::vector<std::wstring> SnapshotClaimedDiskPaths(BackendContext* context, ULONG excludedTargetId) {
    std::vector<std::wstring> paths;

    std::lock_guard<std::mutex> guard(context->disksLock);
    for (const auto& entry : context->disks) {
        if (entry.first == excludedTargetId) {
            continue;
        }
        if (!entry.second->identity.path.empty()) {
            paths.push_back(entry.second->identity.path);
        }
    }

    return paths;
}

bool ContainsPath(const std::vector<std::wstring>& paths, const std::wstring& path) {
    return std::find(paths.begin(), paths.end(), path) != paths.end();
}

bool ContainsVisibleDiskPath(const std::vector<DiskIdentity>& identities, const std::wstring& path) {
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
    const std::vector<DiskIdentity>* baselineVisibleDisks,
    DWORD timeoutMs
) {
    const auto claimedPaths = SnapshotClaimedDiskPaths(context, disk->targetId);
    const ULONGLONG startTick = GetTickCount64();

    for (;;) {
        const auto visibleDisks = EnumerateVisibleYumeDisks(context->config);
        const DiskIdentity* selected = nullptr;

        for (const auto& identity : visibleDisks) {
            if (ContainsPath(claimedPaths, identity.path)) {
                continue;
            }
            if (baselineVisibleDisks != nullptr &&
                ContainsVisibleDiskPath(*baselineVisibleDisks, identity.path)) {
                continue;
            }
            selected = &identity;
            break;
        }

        if (selected == nullptr && !visibleDisks.empty()) {
            for (const auto& identity : visibleDisks) {
                if (!ContainsPath(claimedPaths, identity.path)) {
                    selected = &identity;
                    break;
                }
            }
        }

        if (selected != nullptr) {
            disk->identity = *selected;
            return true;
        }

        if (timeoutMs == 0 ||
            context->stop.load(std::memory_order_relaxed) ||
            (GetTickCount64() - startTick) >= timeoutMs) {
            return false;
        }

        Sleep(kDiskArrivalPollMs);
    }
}

void EnqueueWriteAckRange(ManagedDisk* disk, const YUMEDISK_WRITE_ACK_RANGE& range) {
    std::lock_guard<std::mutex> guard(disk->ackLock);
    disk->pendingWriteAcks.push_back(range);
    disk->pendingWriteAckCount.store(static_cast<uint32_t>(disk->pendingWriteAcks.size()), std::memory_order_relaxed);
}

std::vector<YUMEDISK_WRITE_ACK_RANGE> StealWriteAckRanges(ManagedDisk* disk, size_t maxRanges) {
    std::vector<YUMEDISK_WRITE_ACK_RANGE> ranges;

    std::lock_guard<std::mutex> guard(disk->ackLock);
    while (!disk->pendingWriteAcks.empty() && ranges.size() < maxRanges) {
        ranges.push_back(disk->pendingWriteAcks.front());
        disk->pendingWriteAcks.pop_front();
    }

    disk->pendingWriteAckCount.store(static_cast<uint32_t>(disk->pendingWriteAcks.size()), std::memory_order_relaxed);
    return ranges;
}

void RequeueWriteAckRanges(ManagedDisk* disk, const std::vector<YUMEDISK_WRITE_ACK_RANGE>& ranges) {
    std::lock_guard<std::mutex> guard(disk->ackLock);
    for (auto it = ranges.rbegin(); it != ranges.rend(); ++it) {
        disk->pendingWriteAcks.push_front(*it);
    }
    disk->pendingWriteAckCount.store(static_cast<uint32_t>(disk->pendingWriteAcks.size()), std::memory_order_relaxed);
}

void ClearPendingWriteAcks(ManagedDisk* disk) {
    std::lock_guard<std::mutex> guard(disk->ackLock);
    disk->pendingWriteAcks.clear();
    disk->pendingWriteAckCount.store(0, std::memory_order_relaxed);
}

size_t ComputeWriteAckBatchPayloadLength(size_t rangeCount) {
    if (rangeCount == 0) {
        return 0;
    }

    return static_cast<size_t>(YUMEDISK_WRITE_ACK_BATCH_SIZE(static_cast<UINT32>(rangeCount)));
}

void FillWriteAckBatchPayload(UCHAR* payload, const std::vector<YUMEDISK_WRITE_ACK_RANGE>& ranges) {
    auto* batch = reinterpret_cast<PYUMEDISK_WRITE_ACK_BATCH>(payload);
    batch->RangeCount = static_cast<UINT32>(ranges.size());
    batch->Reserved = 0;
    if (!ranges.empty()) {
        std::memcpy(batch->Ranges, ranges.data(), ranges.size() * sizeof(YUMEDISK_WRITE_ACK_RANGE));
    }
}

bool HandleWriteAckBatchResult(
    BackendContext* context,
    ManagedDisk* disk,
    const YUMEDISK_MESSAGE* message,
    const std::vector<YUMEDISK_WRITE_ACK_RANGE>& ranges
) {
    const auto* result = reinterpret_cast<const YUMEDISK_WRITE_ACK_BATCH_RESULT*>(message->Payload);

    if (message->Header.PayloadLength == 0) {
        return true;
    }

    if (message->Header.PayloadLength < YUMEDISK_WRITE_ACK_BATCH_RESULT_BASE_SIZE) {
        context->stats.protocolFailures.fetch_add(1, std::memory_order_relaxed);
        LogLine(context, L"WRITE_ACK_BATCH returned truncated result payload");
        return false;
    }

    if (result->FailureCount >
        (message->Header.PayloadLength - YUMEDISK_WRITE_ACK_BATCH_RESULT_BASE_SIZE) / sizeof(YUMEDISK_WRITE_ACK_FAILURE)) {
        context->stats.protocolFailures.fetch_add(1, std::memory_order_relaxed);
        LogLine(context, L"WRITE_ACK_BATCH returned invalid failure count");
        return false;
    }

    if (result->FailureCount == 0) {
        return true;
    }

    context->stats.writeAckRangeFailures.fetch_add(result->FailureCount, std::memory_order_relaxed);
    for (UINT32 index = 0; index < result->FailureCount; ++index) {
        const auto& failure = result->Failures[index];
        std::wstring line = L"WRITE_ACK_BATCH range failed";

        line += L", target=" + std::to_wstring(disk->targetId);
        line += L", range_index=" + std::to_wstring(failure.RangeIndex);
        line += L", status=0x" + std::to_wstring(static_cast<unsigned long>(failure.Status));
        if (failure.RangeIndex < ranges.size()) {
            const auto& range = ranges[failure.RangeIndex];
            line += L", event=" + std::to_wstring(range.EventId);
            line += L", seq_base=" + std::to_wstring(range.SeqBase);
            line += L", seq_count=" + std::to_wstring(range.SeqCount);
        }
        LogLine(context, line);
    }

    return true;
}

bool SendWriteAckBatch(
    BackendContext* context,
    ManagedDisk* disk,
    const std::vector<YUMEDISK_WRITE_ACK_RANGE>& ranges
) {
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

    return HandleWriteAckBatchResult(context, disk, message, ranges);
}

bool IsRecoverableSlotError(DWORD error) {
    switch (error) {
    case ERROR_IO_DEVICE:
    case ERROR_NOT_READY:
    case ERROR_OPERATION_ABORTED:
    case ERROR_DEVICE_NOT_CONNECTED:
        return true;
    default:
        return false;
    }
}

bool IsExpectedWorkerStop(BackendContext* context, ManagedDisk* disk) {
    return context->stop.load(std::memory_order_relaxed) || disk->stop.load(std::memory_order_relaxed);
}

void ScheduleRecoverableRetry(SteadyClock::time_point* retryAfter) {
    *retryAfter = SteadyClock::now() + std::chrono::milliseconds(kRecoverableErrorRetryDelayMs);
}

bool IsRetryReady(const SteadyClock::time_point& retryAfter, const SteadyClock::time_point& now) {
    return retryAfter == SteadyClock::time_point::min() || now >= retryAfter;
}

bool PostReadSlotAsync(
    BackendContext* context,
    ManagedDisk* disk,
    ReadSlotContext* slotContext
) {
    DWORD error = ERROR_SUCCESS;
    auto* requestMessage = reinterpret_cast<PYUMEDISK_MESSAGE>(slotContext->requestBuffer.data());

    slotContext->slotId = AllocateTxId(context);
    std::memset(&slotContext->event, 0, sizeof(slotContext->event));
    InitMessageHeader(
        requestMessage,
        static_cast<ULONG>(slotContext->requestBuffer.size()),
        YumeDiskCommandPostReadSlot,
        0,
        context->control.sessionId,
        disk->targetId);
    requestMessage->Header.TxId = slotContext->slotId;

    context->stats.readSlotPosts.fetch_add(1, std::memory_order_relaxed);
    if (!BeginAsyncIoControl(
            context->control.file,
            slotContext->requestBuffer.data(),
            static_cast<DWORD>(slotContext->requestBuffer.size()),
            &slotContext->event,
            sizeof(slotContext->event),
            &slotContext->overlapped,
            &error,
            nullptr)) {
        if (IsExpectedWorkerStop(context, disk)) {
            return true;
        }
        context->stats.commandFailures.fetch_add(1, std::memory_order_relaxed);
        if (IsRecoverableSlotError(error)) {
            ScheduleRecoverableRetry(&slotContext->retryAfter);
            LogLine(context, L"POST_READ_SLOT transient failure, error=" + std::to_wstring(error));
            return true;
        }

        LogLine(context, L"POST_READ_SLOT failed, error=" + std::to_wstring(error));
        return false;
    }

    slotContext->active = true;
    slotContext->retryAfter = SteadyClock::time_point::min();
    disk->activeReadSlots.fetch_add(1, std::memory_order_relaxed);
    return true;
}

bool PostWriteSlotAsync(
    BackendContext* context,
    ManagedDisk* disk,
    WriteSlotContext* slotContext
) {
    DWORD error = ERROR_SUCCESS;
    auto* requestMessage = reinterpret_cast<PYUMEDISK_MESSAGE>(slotContext->requestBuffer.data());

    slotContext->slotId = AllocateTxId(context);
    std::memset(slotContext->slotBuffer.data(), 0, slotContext->slotBuffer.size());
    InitMessageHeader(
        requestMessage,
        static_cast<ULONG>(slotContext->requestBuffer.size()),
        YumeDiskCommandPostWriteSlot,
        0,
        context->control.sessionId,
        disk->targetId);
    requestMessage->Header.TxId = slotContext->slotId;

    context->stats.writeSlotPosts.fetch_add(1, std::memory_order_relaxed);
    if (!BeginAsyncIoControl(
            context->control.file,
            slotContext->requestBuffer.data(),
            static_cast<DWORD>(slotContext->requestBuffer.size()),
            slotContext->slotBuffer.data(),
            static_cast<DWORD>(slotContext->slotBuffer.size()),
            &slotContext->overlapped,
            &error,
            nullptr)) {
        if (IsExpectedWorkerStop(context, disk)) {
            return true;
        }
        context->stats.commandFailures.fetch_add(1, std::memory_order_relaxed);
        if (IsRecoverableSlotError(error)) {
            ScheduleRecoverableRetry(&slotContext->retryAfter);
            LogLine(context, L"POST_WRITE_SLOT transient failure, error=" + std::to_wstring(error));
            return true;
        }

        LogLine(context, L"POST_WRITE_SLOT failed, error=" + std::to_wstring(error));
        return false;
    }

    slotContext->active = true;
    slotContext->retryAfter = SteadyClock::time_point::min();
    disk->activeWriteSlots.fetch_add(1, std::memory_order_relaxed);
    return true;
}

bool ValidateReadSlotEvent(BackendContext* context, ManagedDisk* disk, const YUMEDISK_READ_SLOT_EVENT& event) {
    if (event.EventId == 0 ||
        event.TargetId != disk->targetId ||
        (event.DataLength != 0 && event.DataLength % disk->sectorSize != 0) ||
        (event.BlockCount != 0 &&
            (static_cast<uint64_t>(event.BlockCount) * disk->sectorSize) != event.DataLength)) {
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
    ULONG* dataLength
) {
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
        std::memcpy(readBuffer->data(), disk->medium.data() + offset, event.DataLength);
    }

    return kStatusSuccess;
}

bool HandleReadSlotCompletion(
    BackendContext* context,
    ManagedDisk* disk,
    ReadSlotContext* slotContext
) {
    DWORD error = ERROR_SUCCESS;
    ULONG dataLength = 0;
    auto* readAckRequest = reinterpret_cast<PYUMEDISK_MESSAGE>(slotContext->readAckBuffer.data());
    auto* readAck = reinterpret_cast<PYUMEDISK_READ_ACK>(readAckRequest->Payload);

    slotContext->active = false;
    disk->activeReadSlots.fetch_sub(1, std::memory_order_relaxed);

    if (!FinishAsyncIoControl(context->control.file, &slotContext->overlapped, nullptr, &error)) {
        if (IsExpectedWorkerStop(context, disk)) {
            return true;
        }

        context->stats.commandFailures.fetch_add(1, std::memory_order_relaxed);
        if (IsRecoverableSlotError(error)) {
            ScheduleRecoverableRetry(&slotContext->retryAfter);
            LogLine(context, L"POST_READ_SLOT transient failure, error=" + std::to_wstring(error));
            return true;
        }

        LogLine(context, L"POST_READ_SLOT failed, error=" + std::to_wstring(error));
        return false;
    }

    if (!ValidateReadSlotEvent(context, disk, slotContext->event)) {
        return true;
    }

    const LONG ioStatus = CopyReadData(disk, slotContext->event, &slotContext->readDataBuffer, &dataLength);

    InitMessageHeader(
        readAckRequest,
        static_cast<ULONG>(slotContext->readAckBuffer.size()),
        YumeDiskCommandReadAck,
        sizeof(YUMEDISK_READ_ACK),
        context->control.sessionId,
        disk->targetId);
    readAck->EventId = slotContext->event.EventId;
    readAck->IoStatus = ioStatus;
    readAck->DataLength = dataLength;
    readAck->KernelVa = 0;

    if (!SendIoControl(
            context->control.file,
            slotContext->readAckBuffer.data(),
            static_cast<DWORD>(slotContext->readAckBuffer.size()),
            dataLength == 0 ? nullptr : slotContext->readDataBuffer.data(),
            dataLength,
            nullptr,
            &slotContext->overlapped)) {
        if (IsExpectedWorkerStop(context, disk)) {
            return true;
        }

        error = GetLastError();
        context->stats.commandFailures.fetch_add(1, std::memory_order_relaxed);
        LogLine(context, L"READ_ACK failed, error=" + std::to_wstring(error));
        return false;
    }

    context->stats.readSlotCompletions.fetch_add(1, std::memory_order_relaxed);
    context->stats.readAckCommands.fetch_add(1, std::memory_order_relaxed);
    return true;
}

bool ValidateWriteSlotHeader(
    BackendContext* context,
    ManagedDisk* disk,
    const YUMEDISK_WRITE_SLOT_HEADER& header,
    size_t slotCapacity
) {
    if (header.EventId == 0 ||
        header.TargetId != disk->targetId ||
        header.TotalSeq == 0 ||
        header.Seq >= header.TotalSeq ||
        header.DataLength > slotCapacity - YUMEDISK_WRITE_SLOT_HEADER_BASE_SIZE ||
        header.ByteOffsetInWrite % disk->sectorSize != 0 ||
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
        std::memcpy(disk->medium.data() + offset, header.Data, header.DataLength);
    }

    return kStatusSuccess;
}

bool HandleWriteSlotCompletion(
    BackendContext* context,
    ManagedDisk* disk,
    WriteSlotContext* slotContext,
    size_t slotCapacity
) {
    DWORD error = ERROR_SUCCESS;
    const auto* slotHeader = reinterpret_cast<const YUMEDISK_WRITE_SLOT_HEADER*>(slotContext->slotBuffer.data());

    slotContext->active = false;
    disk->activeWriteSlots.fetch_sub(1, std::memory_order_relaxed);

    if (!FinishAsyncIoControl(context->control.file, &slotContext->overlapped, nullptr, &error)) {
        if (IsExpectedWorkerStop(context, disk)) {
            return true;
        }

        context->stats.commandFailures.fetch_add(1, std::memory_order_relaxed);
        if (IsRecoverableSlotError(error)) {
            ScheduleRecoverableRetry(&slotContext->retryAfter);
            LogLine(context, L"POST_WRITE_SLOT transient failure, error=" + std::to_wstring(error));
            return true;
        }

        LogLine(context, L"POST_WRITE_SLOT failed, error=" + std::to_wstring(error));
        return false;
    }

    if (!ValidateWriteSlotHeader(context, disk, *slotHeader, slotCapacity)) {
        return true;
    }

    const LONG ioStatus = ApplyWriteSlotToMedium(disk, *slotHeader);
    YUMEDISK_WRITE_ACK_RANGE range{};
    range.EventId = slotHeader->EventId;
    range.SeqBase = slotHeader->Seq;
    range.SeqCount = 1;
    range.IoStatus = ioStatus;
    range.Reserved = 0;
    EnqueueWriteAckRange(disk, range);

    context->stats.writeSlotCompletions.fetch_add(1, std::memory_order_relaxed);
    return true;
}

void CancelActiveSlots(
    BackendContext* context,
    ManagedDisk* disk,
    const std::vector<ReadSlotContext>& readContexts,
    const std::vector<WriteSlotContext>& writeContexts
) {
    for (const auto& slotContext : readContexts) {
        if (slotContext.active && slotContext.slotId != 0) {
            (void)CancelSlot(&context->control, disk->targetId, YumeDiskSlotTypeRead, slotContext.slotId);
        }
    }

    for (const auto& slotContext : writeContexts) {
        if (slotContext.active && slotContext.slotId != 0) {
            (void)CancelSlot(&context->control, disk->targetId, YumeDiskSlotTypeWrite, slotContext.slotId);
        }
    }
}

void CloseReadSlotContexts(std::vector<ReadSlotContext>* contexts) {
    for (auto& context : *contexts) {
        CloseOverlappedEvent(&context.overlapped);
    }
}

void CloseWriteSlotContexts(std::vector<WriteSlotContext>* contexts) {
    for (auto& context : *contexts) {
        CloseOverlappedEvent(&context.overlapped);
    }
}

void FlushPendingWriteAcks(
    BackendContext* context,
    ManagedDisk* disk,
    SteadyClock::time_point* nextFlushAt
) {
    std::vector<YUMEDISK_WRITE_ACK_RANGE> ranges;

    if (disk->pendingWriteAckCount.load(std::memory_order_relaxed) == 0) {
        return;
    }

    if (SteadyClock::now() < *nextFlushAt) {
        return;
    }

    ranges = StealWriteAckRanges(disk, kMaxAckBatchRanges);
    if (ranges.empty()) {
        return;
    }

    if (!SendWriteAckBatch(context, disk, ranges)) {
        if (!IsExpectedWorkerStop(context, disk)) {
            RequeueWriteAckRanges(disk, ranges);
            LogLine(context, L"WRITE_ACK_BATCH flush failed");
        }
    } else {
        context->stats.flushedAckCommands.fetch_add(1, std::memory_order_relaxed);
        context->stats.flushedAckRanges.fetch_add(ranges.size(), std::memory_order_relaxed);
    }

    *nextFlushAt = SteadyClock::now() + std::chrono::milliseconds(kWriteAckFlushDelayMs);
}

void RunManagedDiskSlotEngine(BackendContext* context, const std::shared_ptr<ManagedDisk>& disk) {
    const size_t slotCapacity = YUMEDISK_WRITE_SLOT_HEADER_BASE_SIZE + context->config.writeSlotBytes;
    std::vector<ReadSlotContext> readContexts(disk->workerCount);
    std::vector<WriteSlotContext> writeContexts(disk->workerCount);
    bool shuttingDown = false;
    bool cancelIssued = false;
    SteadyClock::time_point nextAckFlushAt = SteadyClock::now() + std::chrono::milliseconds(kWriteAckFlushDelayMs);

    try {
        for (size_t index = 0; index < disk->workerCount; ++index) {
            readContexts[index].requestBuffer = MakeMessageBuffer(YumeDiskCommandPostReadSlot, 0);
            readContexts[index].readAckBuffer = MakeMessageBuffer(YumeDiskCommandReadAck, sizeof(YUMEDISK_READ_ACK));
            if (!EnsureOverlappedEvent(&readContexts[index].overlapped)) {
                throw std::runtime_error("read overlapped event");
            }

            writeContexts[index].requestBuffer = MakeMessageBuffer(YumeDiskCommandPostWriteSlot, 0);
            writeContexts[index].slotBuffer.resize(slotCapacity, 0);
            if (!EnsureOverlappedEvent(&writeContexts[index].overlapped)) {
                throw std::runtime_error("write overlapped event");
            }
        }
    } catch (const std::exception&) {
        context->stats.commandFailures.fetch_add(1, std::memory_order_relaxed);
        CloseReadSlotContexts(&readContexts);
        CloseWriteSlotContexts(&writeContexts);
        return;
    }

    for (;;) {
        const bool stopRequested = shuttingDown || IsExpectedWorkerStop(context, disk.get());
        const SteadyClock::time_point now = SteadyClock::now();
        std::vector<HANDLE> waitHandles;
        std::vector<std::pair<bool, size_t>> waitKeys;

        if (!stopRequested) {
            for (size_t index = 0; index < readContexts.size(); ++index) {
                auto& slotContext = readContexts[index];
                if (slotContext.active || !IsRetryReady(slotContext.retryAfter, now)) {
                    continue;
                }

                if (!PostReadSlotAsync(context, disk.get(), &slotContext)) {
                    shuttingDown = true;
                    disk->stop.store(true, std::memory_order_relaxed);
                    break;
                }
            }

            if (!shuttingDown) {
                for (size_t index = 0; index < writeContexts.size(); ++index) {
                    auto& slotContext = writeContexts[index];
                    if (slotContext.active || !IsRetryReady(slotContext.retryAfter, now)) {
                        continue;
                    }

                    if (!PostWriteSlotAsync(context, disk.get(), &slotContext)) {
                        shuttingDown = true;
                        disk->stop.store(true, std::memory_order_relaxed);
                        break;
                    }
                }
            }

            if (!shuttingDown) {
                FlushPendingWriteAcks(context, disk.get(), &nextAckFlushAt);
            }
        } else {
            ClearPendingWriteAcks(disk.get());
            if (!cancelIssued) {
                CancelActiveSlots(context, disk.get(), readContexts, writeContexts);
                cancelIssued = true;
            }
        }

        for (size_t index = 0; index < readContexts.size(); ++index) {
            if (!readContexts[index].active) {
                continue;
            }
            waitHandles.push_back(readContexts[index].overlapped.hEvent);
            waitKeys.emplace_back(true, index);
        }

        for (size_t index = 0; index < writeContexts.size(); ++index) {
            if (!writeContexts[index].active) {
                continue;
            }
            waitHandles.push_back(writeContexts[index].overlapped.hEvent);
            waitKeys.emplace_back(false, index);
        }

        if ((stopRequested || shuttingDown) && waitHandles.empty()) {
            break;
        }

        if (waitHandles.empty()) {
            Sleep(kSlotEnginePollMs);
            continue;
        }

        const DWORD waitStatus = WaitForMultipleObjects(
            static_cast<DWORD>(waitHandles.size()),
            waitHandles.data(),
            FALSE,
            kSlotEnginePollMs);
        if (waitStatus == WAIT_TIMEOUT) {
            continue;
        }

        if (waitStatus == WAIT_FAILED) {
            if (!stopRequested) {
                context->stats.commandFailures.fetch_add(1, std::memory_order_relaxed);
                LogLine(context, L"slot engine wait failed, error=" + std::to_wstring(GetLastError()));
                shuttingDown = true;
                disk->stop.store(true, std::memory_order_relaxed);
            }
            continue;
        }

        const size_t completedIndex = static_cast<size_t>(waitStatus - WAIT_OBJECT_0);
        if (completedIndex >= waitKeys.size()) {
            if (!stopRequested) {
                context->stats.commandFailures.fetch_add(1, std::memory_order_relaxed);
                LogLine(context, L"slot engine wait returned invalid index");
                shuttingDown = true;
                disk->stop.store(true, std::memory_order_relaxed);
            }
            continue;
        }

        const auto [isRead, slotIndex] = waitKeys[completedIndex];
        const bool completedOk = isRead
            ? HandleReadSlotCompletion(context, disk.get(), &readContexts[slotIndex])
            : HandleWriteSlotCompletion(context, disk.get(), &writeContexts[slotIndex], slotCapacity);
        if (!completedOk) {
            shuttingDown = true;
            disk->stop.store(true, std::memory_order_relaxed);
        }
    }

    ClearPendingWriteAcks(disk.get());
    CloseReadSlotContexts(&readContexts);
    CloseWriteSlotContexts(&writeContexts);
}

bool StartManagedDiskWorkers(BackendContext* context, const std::shared_ptr<ManagedDisk>& disk) {
    try {
        disk->slotEngineThread = std::thread(RunManagedDiskSlotEngine, context, disk);
    } catch (const std::exception&) {
        return false;
    }

    return true;
}

void SignalManagedDiskStop(ManagedDisk* disk, bool stop) {
    if (disk == nullptr) {
        return;
    }

    disk->stop.store(stop, std::memory_order_relaxed);
}

void JoinManagedDiskWorkers(ManagedDisk* disk) {
    if (disk != nullptr && disk->slotEngineThread.joinable()) {
        disk->slotEngineThread.join();
    }
}

void PrintBackendStats(const BackendContext* context) {
    std::wcout << L"backend_read_slot_posts=" << context->stats.readSlotPosts.load(std::memory_order_relaxed)
               << L", backend_read_slot_completions=" << context->stats.readSlotCompletions.load(std::memory_order_relaxed)
               << L", backend_read_ack_commands=" << context->stats.readAckCommands.load(std::memory_order_relaxed)
               << L", backend_write_slot_posts=" << context->stats.writeSlotPosts.load(std::memory_order_relaxed)
               << L", backend_write_slot_completions=" << context->stats.writeSlotCompletions.load(std::memory_order_relaxed)
               << L", backend_flushed_ack_commands=" << context->stats.flushedAckCommands.load(std::memory_order_relaxed)
               << L", backend_flushed_ack_ranges=" << context->stats.flushedAckRanges.load(std::memory_order_relaxed)
               << L", backend_write_ack_range_failures=" << context->stats.writeAckRangeFailures.load(std::memory_order_relaxed)
               << L", backend_command_failures=" << context->stats.commandFailures.load(std::memory_order_relaxed)
               << L", backend_protocol_failures=" << context->stats.protocolFailures.load(std::memory_order_relaxed)
               << std::endl;
}

void PrintScsiDebugState(BackendContext* context) {
    YUMEDISK_DEBUG_STATE debugState{};

    if (!QueryDebugState(&context->control, &debugState)) {
        std::wcerr << L"debug_scsi query_failed=true, last_error=" << GetLastError() << std::endl;
        return;
    }

    std::wcout << L"debug_scsi active_session=" << debugState.ActiveSessionId
               << L", progress=" << debugState.ProgressCounter
               << L", posted_read_slots=" << debugState.PostedReadSlots
               << L", pending_reads=" << debugState.PendingReads
               << L", pending_reads_issued=" << debugState.PendingReadsIssued
               << L", posted_write_slots=" << debugState.PostedWriteSlots
               << L", pending_writes=" << debugState.PendingWrites
               << L", pending_write_fragments_issued=" << debugState.PendingWriteFragmentsIssued
               << L", pending_write_fragments_acked=" << debugState.PendingWriteFragmentsAcked
               << L", read_queued=" << debugState.ReadRequestsQueued
               << L", read_slots_issued_total=" << debugState.ReadSlotsIssued
               << L", read_acks_applied_total=" << debugState.ReadAcksApplied
               << L", read_completed=" << debugState.ReadRequestsCompleted
               << L", read_failed=" << debugState.ReadRequestsFailed
               << L", write_queued=" << debugState.WriteRequestsQueued
               << L", write_fragments_issued_total=" << debugState.WriteFragmentsIssued
               << L", write_acks_applied_total=" << debugState.WriteAcksApplied
               << L", write_completed=" << debugState.WriteRequestsCompleted
               << L", write_failed=" << debugState.WriteRequestsFailed
               << std::endl;
}

void PrintDebugSnapshot(BackendContext* context, const wchar_t* reason) {
    std::wcout << L"debug_snapshot reason=" << reason << std::endl;
    PrintBackendStats(context);

    const auto disks = SnapshotManagedDisks(context);
    for (const auto& disk : disks) {
        std::wcout << L"debug_disk target=" << disk->targetId
                   << L", pending_write_acks=" << disk->pendingWriteAckCount.load(std::memory_order_relaxed)
                   << L", active_read_slots=" << disk->activeReadSlots.load(std::memory_order_relaxed)
                   << L", active_write_slots=" << disk->activeWriteSlots.load(std::memory_order_relaxed)
                   << std::endl;
    }

    PrintScsiDebugState(context);
}

void PrintRuntimeHelp() {
    std::cout
        << "commands:\n"
        << "  help           show commands\n"
        << "  query          query control protocol info\n"
        << "  ct [target]    create one disk target and start its slot engine\n"
        << "  rm <target>    remove one disk target and stop its slot engine\n"
        << "  rm all         remove all disk targets and stop all slot engines\n"
        << "  ls             list managed targets and visible YumeDisk disks\n"
        << "  stats          print app backend counters\n"
        << "  debug          print app plus SCSI queue snapshot\n"
        << "  exit           close session and quit\n"
        << "\n"
        << "runtime:\n"
        << "  heartbeat stays in the background while the app is running\n"
        << "  each created disk gets one slot engine thread and per-disk read/write slot depth\n"
        << "  write slot bytes are fixed per disk for the whole session\n";
}

void PrintUsage() {
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
            if (!nextValue(&value) ||
                value == 0 ||
                value > std::numeric_limits<size_t>::max() ||
                value > kMaxSlotEngineQueueDepth) {
                return ParseResult::Error;
            }
            config->queueDepth = static_cast<size_t>(value);
            continue;
        }
        if (arg == "--slot-bytes") {
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

    if (config->queueDepth == 0 ||
        config->writeSlotBytes < config->sectorSize ||
        config->writeSlotBytes % config->sectorSize != 0 ||
        config->diskSizeBytes == 0 ||
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
    std::wcout << L"managed_target_count=" << managedDisks.size() << std::endl;
    for (const auto& disk : managedDisks) {
        (void)TryRefreshManagedDiskIdentity(context, disk, nullptr, 0);
        const std::wstring physicalDrive = MakePhysicalDrivePath(disk->identity.deviceNumber);
        std::wcout << L"target=" << disk->targetId
                   << L", disk_bytes=" << disk->diskSizeBytes
                   << L", sector_size=" << disk->sectorSize
                   << L", slot_engine=" << (disk->slotEngineThread.joinable() ? L"running" : L"stopped")
                   << L", read_slot_depth=" << disk->workerCount
                   << L", write_slot_depth=" << disk->workerCount
                   << L", visible_path=" << (disk->identity.path.empty() ? L"<pending-enumeration>" : disk->identity.path)
                   << L", physical_drive=" << (physicalDrive.empty() ? L"<pending-enumeration>" : physicalDrive)
                   << std::endl;
    }

    const auto visibleDisks = EnumerateVisibleYumeDisks(context->config);
    std::wcout << L"visible_disk_count=" << visibleDisks.size() << std::endl;
    for (size_t index = 0; index < visibleDisks.size(); ++index) {
        const auto& disk = visibleDisks[index];
        std::wcout << L"visible_disk[" << index << L"]"
                   << L", path=" << disk.path
                   << L", device_number=" << disk.deviceNumber
                   << L", disk_bytes=" << disk.lengthBytes
                   << std::endl;
    }
}

bool CreateManagedDisk(BackendContext* context, ULONG targetId) {
    std::shared_ptr<ManagedDisk> disk;
    const auto visibleDisksBeforeCreate = EnumerateVisibleYumeDisks(context->config);

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
        disk->workerCount = context->config.queueDepth;
        disk->medium.resize(static_cast<size_t>(context->config.diskSizeBytes), 0);
    } catch (const std::exception&) {
        (void)RemoveDisk(&context->control, targetId);
        std::wcerr << L"create failed, target=" << targetId << L", reason=memory-allocation" << std::endl;
        return false;
    }

    if (!StartManagedDiskWorkers(context, disk)) {
        SignalManagedDiskStop(disk.get(), true);
        (void)RemoveDisk(&context->control, targetId);
        JoinManagedDiskWorkers(disk.get());
        std::wcerr << L"create failed, target=" << targetId << L", reason=slot-engine-start" << std::endl;
        return false;
    }

    InsertManagedDisk(context, disk);
    std::wcout << L"created target=" << targetId
               << L", queue_depth=" << context->config.queueDepth
               << L", slot_bytes=" << context->config.writeSlotBytes
               << std::endl;
    if (TryRefreshManagedDiskIdentity(context, disk, &visibleDisksBeforeCreate, kDiskArrivalTimeoutMs)) {
        std::wcout << L"visible_path=" << disk->identity.path
                   << L", physical_drive=" << MakePhysicalDrivePath(disk->identity.deviceNumber)
                   << std::endl;
    } else {
        std::wcout << L"visible_path=<pending-enumeration>, target=" << targetId << std::endl;
    }
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
        if (command == "debug") {
            PrintDebugSnapshot(context, L"manual");
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

    backend.control.file = OpenFirstDeviceInterface(&GUID_YUMEDISK_CONTROL);
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

    (void)RemoveAllManagedDisks(&backend, true);
    heartbeatThread.join();
    PrintBackendStats(&backend);

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
