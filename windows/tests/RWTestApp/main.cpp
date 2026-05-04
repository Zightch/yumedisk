#define NOMINMAX

#include <Windows.h>
#include <SetupAPI.h>
#include <Ntddstor.h>
#include <WinIoCtl.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <limits>
#include <map>
#include <mutex>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "yumedisk_proto.h"

namespace {

constexpr LONG kStatusSuccess = 0x00000000L;
constexpr LONG kStatusInvalidParameter = static_cast<LONG>(0xC000000DL);
constexpr LONG kStatusBufferTooSmall = static_cast<LONG>(0xC0000023L);
constexpr LONG kStatusDeviceNotConnected = static_cast<LONG>(0xC000009DL);

constexpr ULONG kDefaultTargetId = 0;
constexpr ULONG kDefaultSectorSize = 4096;
constexpr size_t kDefaultQueueDepth = 64;
constexpr size_t kDefaultIoSize = 64 * 1024;
constexpr size_t kDefaultWaitInlineDataCapacity = 8 * 1024 * 1024;
constexpr uint64_t kDefaultDiskSizeBytes = 64ull * 1024ull * 1024ull;

struct AppConfig {
    ULONG targetId = kDefaultTargetId;
    ULONG sectorSize = kDefaultSectorSize;
    size_t queueDepth = kDefaultQueueDepth;
    size_t ioSize = kDefaultIoSize;
    size_t waitInlineDataCapacity = kDefaultWaitInlineDataCapacity;
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
    std::atomic<uint64_t> waitCompletions{0};
    std::atomic<uint64_t> syntheticEvents{0};
    std::atomic<uint64_t> readEvents{0};
    std::atomic<uint64_t> writeEvents{0};
    std::atomic<uint64_t> readReplies{0};
    std::atomic<uint64_t> writeAcks{0};
    std::atomic<uint64_t> commandFailures{0};
    std::atomic<uint64_t> protocolFailures{0};
};

struct ManagedDisk {
    ULONG targetId = 0;
    DiskIdentity identity{};
    std::vector<unsigned char> medium;
};

struct BackendContext;

struct WorkerContext {
    BackendContext* backend = nullptr;
    std::vector<unsigned char> waitBuffer;
    std::vector<unsigned char> readReplyBuffer;
    std::vector<unsigned char> writeAckBuffer;
    OVERLAPPED overlapped{};
};

struct BackendContext {
    ControlContext control;
    AppConfig config;
    std::map<ULONG, ManagedDisk> disks;
    std::atomic<bool> stop{false};
    BackendStats stats;
    std::mutex diskLock;
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

void LogLine(BackendContext& context, const std::wstring& text) {
    std::lock_guard<std::mutex> guard(context.logLock);
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

bool SendCommand(
    HANDLE file,
    std::vector<unsigned char>& buffer,
    DWORD* bytesReturned = nullptr,
    OVERLAPPED* reusedOverlapped = nullptr
) {
    OVERLAPPED localOverlapped{};
    DWORD transferred = 0;
    OVERLAPPED* overlapped = reusedOverlapped;
    bool ownsEvent = false;

    if (overlapped == nullptr) {
        localOverlapped.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (localOverlapped.hEvent == nullptr) {
            return false;
        }
        overlapped = &localOverlapped;
        ownsEvent = true;
    } else {
        const HANDLE eventHandle = overlapped->hEvent;
        OVERLAPPED reset{};
        reset.hEvent = eventHandle;
        *overlapped = reset;
        if (eventHandle != nullptr) {
            ResetEvent(eventHandle);
        }
    }

    BOOL ok = DeviceIoControl(
        file,
        IOCTL_YUMEDISK_APP_COMMAND,
        nullptr,
        0,
        buffer.data(),
        static_cast<DWORD>(buffer.size()),
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

    if (ownsEvent) {
        CloseHandle(overlapped->hEvent);
    }
    if (!ok) {
        return false;
    }

    if (bytesReturned != nullptr) {
        *bytesReturned = transferred;
    }
    return true;
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

bool CreateDisk(ControlContext* control, const AppConfig& config) {
    auto buffer = MakeMessageBuffer(YumeDiskCommandCreateDisk, sizeof(YUMEDISK_CREATE_DISK));
    auto* message = reinterpret_cast<PYUMEDISK_MESSAGE>(buffer.data());
    auto* request = reinterpret_cast<PYUMEDISK_CREATE_DISK>(message->Payload);

    message->Header.SessionId = control->sessionId;
    request->TargetId = config.targetId;
    request->SectorSize = config.sectorSize;
    request->SectorCount = config.diskSizeBytes / config.sectorSize;
    request->DiskSizeBytes = config.diskSizeBytes;

    if (!SendCommand(control->file, buffer, nullptr)) {
        return false;
    }

    return message->Header.Status == kStatusSuccess;
}

bool CreateDisk(ControlContext* control, const AppConfig& config, ULONG targetId) {
    AppConfig localConfig = config;
    localConfig.targetId = targetId;
    return CreateDisk(control, localConfig);
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

bool SendWriteAck(BackendContext* context, uint64_t txId, LONG ioStatus) {
    auto buffer = MakeMessageBuffer(YumeDiskCommandWriteAck, sizeof(YUMEDISK_WRITE_ACK));
    auto* message = reinterpret_cast<PYUMEDISK_MESSAGE>(buffer.data());
    auto* ack = reinterpret_cast<PYUMEDISK_WRITE_ACK>(message->Payload);

    message->Header.SessionId = context->control.sessionId;
    ack->TxId = txId;
    ack->IoStatus = ioStatus;

    if (!SendCommand(context->control.file, buffer, nullptr)) {
        context->stats.commandFailures.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    if (message->Header.Status != kStatusSuccess) {
        context->stats.protocolFailures.fetch_add(1, std::memory_order_relaxed);
        return false;
    }

    context->stats.writeAcks.fetch_add(1, std::memory_order_relaxed);
    return true;
}

bool SendWorkerCommand(WorkerContext* worker, std::vector<unsigned char>& buffer, DWORD* bytesReturned = nullptr) {
    return SendCommand(worker->backend->control.file, buffer, bytesReturned, &worker->overlapped);
}

void EnsureReadReplyBufferCapacity(WorkerContext* worker, ULONG dataLength) {
    const size_t requiredSize =
        static_cast<size_t>(YUMEDISK_MESSAGE_BASE_SIZE + YUMEDISK_READ_REPLY_BASE_SIZE + dataLength);
    if (worker->readReplyBuffer.size() < requiredSize) {
        worker->readReplyBuffer.resize(requiredSize, 0);
    }
}

bool SendWriteAck(
    WorkerContext* worker,
    uint64_t txId,
    LONG ioStatus
) {
    auto& buffer = worker->writeAckBuffer;
    auto* message = reinterpret_cast<PYUMEDISK_MESSAGE>(buffer.data());
    auto* ack = reinterpret_cast<PYUMEDISK_WRITE_ACK>(message->Payload);

    message->Header.Size = static_cast<ULONG>(YUMEDISK_MESSAGE_BASE_SIZE + sizeof(YUMEDISK_WRITE_ACK));
    message->Header.Version = YUMEDISK_PROTOCOL_VERSION;
    message->Header.Command = YumeDiskCommandWriteAck;
    message->Header.PayloadLength = sizeof(YUMEDISK_WRITE_ACK);
    message->Header.SessionId = worker->backend->control.sessionId;
    ack->TxId = txId;
    ack->IoStatus = ioStatus;
    ack->Reserved0 = 0;
    ack->Reserved1 = 0;

    if (!SendWorkerCommand(worker, buffer, nullptr)) {
        worker->backend->stats.commandFailures.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    if (message->Header.Status != kStatusSuccess) {
        worker->backend->stats.protocolFailures.fetch_add(1, std::memory_order_relaxed);
        return false;
    }

    worker->backend->stats.writeAcks.fetch_add(1, std::memory_order_relaxed);
    return true;
}

bool SendReadReply(
    WorkerContext* worker,
    uint64_t txId,
    LONG ioStatus,
    const unsigned char* data,
    ULONG dataLength
) {
    auto& buffer = worker->readReplyBuffer;
    auto* message = reinterpret_cast<PYUMEDISK_MESSAGE>(buffer.data());
    auto* reply = reinterpret_cast<PYUMEDISK_READ_REPLY>(message->Payload);

    message->Header.Size = static_cast<ULONG>(YUMEDISK_MESSAGE_BASE_SIZE + YUMEDISK_READ_REPLY_BASE_SIZE + dataLength);
    message->Header.Version = YUMEDISK_PROTOCOL_VERSION;
    message->Header.Command = YumeDiskCommandReadReply;
    message->Header.PayloadLength = static_cast<ULONG>(YUMEDISK_READ_REPLY_BASE_SIZE + dataLength);
    message->Header.SessionId = worker->backend->control.sessionId;
    reply->TxId = txId;
    reply->IoStatus = ioStatus;
    reply->DataLength = dataLength;
    reply->Reserved = 0;
    if (dataLength != 0 && data != nullptr) {
        std::memcpy(reply->Data, data, dataLength);
    }

    if (!SendWorkerCommand(worker, buffer, nullptr)) {
        worker->backend->stats.commandFailures.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    if (message->Header.Status != kStatusSuccess) {
        worker->backend->stats.protocolFailures.fetch_add(1, std::memory_order_relaxed);
        return false;
    }

    worker->backend->stats.readReplies.fetch_add(1, std::memory_order_relaxed);
    return true;
}

void ProcessWriteEvent(WorkerContext* worker, const YUMEDISK_EVENT& event, const unsigned char* inlineData) {
    BackendContext* context = worker->backend;
    LONG ioStatus = kStatusSuccess;
    {
        std::lock_guard<std::mutex> guard(context->diskLock);
        const auto diskIt = context->disks.find(event.TargetId);
        if (diskIt == context->disks.end()) {
            ioStatus = kStatusDeviceNotConnected;
        } else {
            const uint64_t offset = event.Lba * static_cast<uint64_t>(context->config.sectorSize);
            auto& medium = diskIt->second.medium;
            if (offset > medium.size() ||
                       static_cast<uint64_t>(event.DataLength) > medium.size() - offset) {
                ioStatus = kStatusInvalidParameter;
            } else if (event.DataLength != 0 && inlineData == nullptr) {
                ioStatus = kStatusInvalidParameter;
            } else if (event.DataLength != 0) {
                std::memcpy(medium.data() + offset, inlineData, event.DataLength);
            }
        }
    }

    if (!SendWriteAck(worker, event.TxId, ioStatus) && !context->stop.load(std::memory_order_relaxed)) {
        LogLine(*context, L"WRITE_ACK failed");
    }
}

void ProcessReadEvent(WorkerContext* worker, const YUMEDISK_EVENT& event) {
    BackendContext* context = worker->backend;
    LONG ioStatus = kStatusSuccess;
    ULONG dataLength = event.DataLength;

    EnsureReadReplyBufferCapacity(worker, dataLength);
    auto& replyBuffer = worker->readReplyBuffer;
    auto* message = reinterpret_cast<PYUMEDISK_MESSAGE>(replyBuffer.data());
    auto* reply = reinterpret_cast<PYUMEDISK_READ_REPLY>(message->Payload);

    {
        std::lock_guard<std::mutex> guard(context->diskLock);
        const auto diskIt = context->disks.find(event.TargetId);
        if (diskIt == context->disks.end()) {
            ioStatus = kStatusDeviceNotConnected;
            dataLength = 0;
        } else {
            const uint64_t offset = event.Lba * static_cast<uint64_t>(context->config.sectorSize);
            const auto& medium = diskIt->second.medium;
            if (offset > medium.size() ||
                       static_cast<uint64_t>(event.DataLength) > medium.size() - offset) {
                ioStatus = kStatusInvalidParameter;
                dataLength = 0;
            } else if (event.DataLength != 0) {
                std::memcpy(reply->Data, medium.data() + offset, event.DataLength);
            }
        }
    }

    if (!SendReadReply(
            worker,
            event.TxId,
            ioStatus,
            (dataLength == 0) ? nullptr : reply->Data,
            dataLength) &&
        !context->stop.load(std::memory_order_relaxed)) {
        LogLine(*context, L"READ_REPLY failed");
    }
}

void WaitWorker(BackendContext* context) {
    WorkerContext worker{};
    bool shouldExit = false;
    worker.backend = context;
    worker.waitBuffer = MakeMessageBuffer(
        YumeDiskCommandWaitEvent,
        sizeof(YUMEDISK_WAIT_EVENT),
        static_cast<ULONG>(
            sizeof(YUMEDISK_WAIT_EVENT) +
            sizeof(YUMEDISK_EVENT) +
            context->config.waitInlineDataCapacity));
    worker.readReplyBuffer = MakeMessageBuffer(
        YumeDiskCommandReadReply,
        static_cast<ULONG>(YUMEDISK_READ_REPLY_BASE_SIZE),
        static_cast<ULONG>(YUMEDISK_READ_REPLY_BASE_SIZE + context->config.ioSize));
    worker.writeAckBuffer = MakeMessageBuffer(YumeDiskCommandWriteAck, sizeof(YUMEDISK_WRITE_ACK));
    worker.overlapped.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (worker.overlapped.hEvent == nullptr) {
        context->stats.commandFailures.fetch_add(1, std::memory_order_relaxed);
        return;
    }

    auto* message = reinterpret_cast<PYUMEDISK_MESSAGE>(worker.waitBuffer.data());
    auto* waitRequest = reinterpret_cast<PYUMEDISK_WAIT_EVENT>(message->Payload);

    while (!context->stop.load(std::memory_order_relaxed)) {
        std::memset(worker.waitBuffer.data(), 0, worker.waitBuffer.size());
        message = reinterpret_cast<PYUMEDISK_MESSAGE>(worker.waitBuffer.data());
        waitRequest = reinterpret_cast<PYUMEDISK_WAIT_EVENT>(message->Payload);
        message->Header.Size = static_cast<ULONG>(worker.waitBuffer.size());
        message->Header.Version = YUMEDISK_PROTOCOL_VERSION;
        message->Header.Command = YumeDiskCommandWaitEvent;
        message->Header.PayloadLength = sizeof(YUMEDISK_WAIT_EVENT);
        message->Header.SessionId = context->control.sessionId;
        waitRequest->TimeoutMs = 0;

        DWORD bytesReturned = 0;
        if (!SendWorkerCommand(&worker, worker.waitBuffer, &bytesReturned)) {
            if (!context->stop.load(std::memory_order_relaxed)) {
                context->stats.commandFailures.fetch_add(1, std::memory_order_relaxed);
            }
            break;
        }

        if (message->Header.Status != kStatusSuccess) {
            if (message->Header.Status == kStatusBufferTooSmall) {
                LogLine(
                    *context,
                    L"WAIT_EVENT buffer too small; increase wait inline capacity if the benchmark issues large writes");
            }
            if (context->stop.load(std::memory_order_relaxed) &&
                message->Header.Status == kStatusDeviceNotConnected) {
                break;
            }

            context->stats.protocolFailures.fetch_add(1, std::memory_order_relaxed);
            continue;
        }
        if (message->Header.PayloadLength < sizeof(YUMEDISK_EVENT) || bytesReturned < YUMEDISK_MESSAGE_BASE_SIZE) {
            context->stats.protocolFailures.fetch_add(1, std::memory_order_relaxed);
            continue;
        }

        const auto* event = reinterpret_cast<const YUMEDISK_EVENT*>(message->Payload);
        const auto* inlineData = message->Payload + sizeof(YUMEDISK_EVENT);

        context->stats.waitCompletions.fetch_add(1, std::memory_order_relaxed);
        switch (event->EventType) {
        case YumeDiskEventReadRequest:
            context->stats.readEvents.fetch_add(1, std::memory_order_relaxed);
            ProcessReadEvent(&worker, *event);
            break;
        case YumeDiskEventWriteRequest:
            context->stats.writeEvents.fetch_add(1, std::memory_order_relaxed);
            ProcessWriteEvent(&worker, *event, inlineData);
            break;
        case YumeDiskEventDiskAdded:
        case YumeDiskEventDiskRemoved:
            context->stats.syntheticEvents.fetch_add(1, std::memory_order_relaxed);
            break;
        case YumeDiskEventShutdown:
            context->stats.syntheticEvents.fetch_add(1, std::memory_order_relaxed);
            context->stop.store(true, std::memory_order_relaxed);
            shouldExit = true;
            break;
        default:
            context->stats.protocolFailures.fetch_add(1, std::memory_order_relaxed);
            break;
        }

        if (shouldExit) {
            break;
        }
    }

    CloseHandle(worker.overlapped.hEvent);
}

void PrintRuntimeHelp() {
    std::cout
        << "commands:\n"
        << "  help           show commands\n"
        << "  ct [target]    create one disk on target, default picks first free target\n"
        << "  rm <target>    remove one disk\n"
        << "  rm all         remove all disks\n"
        << "  ls             list managed disks\n"
        << "  exit           stop app and remove all disks\n";
}

void PrintUsage() {
    std::cout
        << "RWTestApp [--queue-depth N] [--io-size BYTES] [--wait-inline-mb MB]\n"
        << "          [--disk-size-mb MB] [--sector-size BYTES] [--target ID]\n"
        << "\n"
        << "defaults:\n"
        << "  queue-depth  = " << kDefaultQueueDepth << "\n"
        << "  io-size      = " << kDefaultIoSize << "\n"
        << "  wait-inline-mb = " << (kDefaultWaitInlineDataCapacity / (1024ull * 1024ull)) << "\n"
        << "  disk-size-mb = " << (kDefaultDiskSizeBytes / (1024ull * 1024ull)) << "\n"
        << "  sector-size  = " << kDefaultSectorSize << "\n"
        << "  target       = " << kDefaultTargetId << "\n"
        << "\n"
        << "behavior:\n"
        << "  starts the YumeDisk backend, then accepts ct/rm/ls/exit commands\n";
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
        if (arg == "--io-size") {
            if (!nextValue(&value) || value == 0 || value > std::numeric_limits<size_t>::max()) {
                return ParseResult::Error;
            }
            config->ioSize = static_cast<size_t>(value);
            continue;
        }
        if (arg == "--wait-inline-mb") {
            if (!nextValue(&value) || value == 0 || value > (std::numeric_limits<size_t>::max() / (1024ull * 1024ull))) {
                return ParseResult::Error;
            }
            config->waitInlineDataCapacity = static_cast<size_t>(value * 1024ull * 1024ull);
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

    if (config->ioSize % config->sectorSize != 0) {
        return ParseResult::Error;
    }
    if (config->waitInlineDataCapacity < config->ioSize) {
        config->waitInlineDataCapacity = config->ioSize;
    }
    if (config->diskSizeBytes % config->sectorSize != 0 || config->diskSizeBytes % config->ioSize != 0) {
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

ULONG FindFirstFreeTarget(BackendContext* context) {
    std::lock_guard<std::mutex> guard(context->diskLock);
    for (ULONG targetId = YUMEDISK_MIN_TARGET_ID; targetId <= YUMEDISK_MAX_USABLE_TARGET_ID; ++targetId) {
        if (context->disks.find(targetId) == context->disks.end()) {
            return targetId;
        }
    }

    return YUMEDISK_MAX_TARGETS;
}

void ListManagedDisks(BackendContext* context) {
    std::vector<ManagedDisk> managedDisks;
    {
        std::lock_guard<std::mutex> guard(context->diskLock);
        managedDisks.reserve(context->disks.size());
        for (const auto& entry : context->disks) {
            managedDisks.push_back(entry.second);
        }
    }

    if (managedDisks.empty()) {
        std::wcout << L"disk_count=0" << std::endl;
    } else {
        std::wcout << L"disk_count=" << managedDisks.size() << std::endl;
        for (const auto& disk : managedDisks) {
            std::wcout << L"target=" << disk.targetId
                       << L", path=" << (disk.identity.path.empty() ? L"<unknown>" : disk.identity.path)
                       << L", device_number=" << disk.identity.deviceNumber
                       << L", disk_bytes=" << (disk.identity.lengthBytes == 0 ? context->config.diskSizeBytes : disk.identity.lengthBytes)
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
    ManagedDisk disk{};
    disk.targetId = targetId;
    disk.medium.resize(static_cast<size_t>(context->config.diskSizeBytes), 0);

    {
        std::lock_guard<std::mutex> guard(context->diskLock);
        if (context->disks.find(targetId) != context->disks.end()) {
            std::wcerr << L"create failed, target already exists: " << targetId << std::endl;
            return false;
        }

        context->disks.emplace(targetId, std::move(disk));
    }

    if (!CreateDisk(&context->control, context->config, targetId)) {
        std::lock_guard<std::mutex> guard(context->diskLock);
        context->disks.erase(targetId);
        std::wcerr << L"create failed, target=" << targetId << std::endl;
        return false;
    }

    std::wcout << L"created target=" << targetId
               << L", path=<pending-enumeration>"
               << std::endl;
    return true;
}

bool RemoveManagedDisk(BackendContext* context, ULONG targetId) {
    {
        std::lock_guard<std::mutex> guard(context->diskLock);
        if (context->disks.find(targetId) == context->disks.end()) {
            std::wcerr << L"remove failed, target not found: " << targetId << std::endl;
            return false;
        }
    }

    if (!RemoveDisk(&context->control, targetId)) {
        std::wcerr << L"remove failed, target=" << targetId << std::endl;
        return false;
    }

    {
        std::lock_guard<std::mutex> guard(context->diskLock);
        context->disks.erase(targetId);
    }

    std::wcout << L"removed target=" << targetId << std::endl;
    return true;
}

bool RemoveAllManagedDisks(BackendContext* context, bool closeSession = false) {
    const ULONG flags = closeSession ? YUMEDISK_SESSION_CLOSE_FLAG : 0;
    if (!RemoveAllDisks(&context->control, flags)) {
        std::wcerr << L"remove all failed" << std::endl;
        return false;
    }

    {
        std::lock_guard<std::mutex> guard(context->diskLock);
        context->disks.clear();
    }

    std::wcout << L"removed_all=true" << std::endl;
    return true;
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
        if (command == "ls") {
            ListManagedDisks(context);
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

    std::vector<std::thread> workers;
    workers.reserve(config.queueDepth);
    for (size_t i = 0; i < config.queueDepth; ++i) {
        workers.emplace_back(WaitWorker, &backend);
    }

    std::wcout << L"control_session=" << backend.control.sessionId << std::endl;
    std::wcout << L"queue_depth=" << config.queueDepth
               << L", io_size=" << config.ioSize
               << L", wait_inline_bytes=" << config.waitInlineDataCapacity
               << L", sector_size=" << config.sectorSize
               << L", disk_bytes=" << config.diskSizeBytes
               << std::endl;
    std::wcout << L"state=ready" << std::endl;

    RunCommandLoop(&backend);
    WaitForSingleObject(g_StopEvent, INFINITE);
    backend.stop.store(true, std::memory_order_relaxed);

    std::wcout << L"backend_wait_completions=" << backend.stats.waitCompletions.load(std::memory_order_relaxed)
               << L", backend_read_events=" << backend.stats.readEvents.load(std::memory_order_relaxed)
               << L", backend_write_events=" << backend.stats.writeEvents.load(std::memory_order_relaxed)
               << L", backend_read_replies=" << backend.stats.readReplies.load(std::memory_order_relaxed)
               << L", backend_write_acks=" << backend.stats.writeAcks.load(std::memory_order_relaxed)
               << L", backend_synthetic_events=" << backend.stats.syntheticEvents.load(std::memory_order_relaxed)
               << L", backend_command_failures=" << backend.stats.commandFailures.load(std::memory_order_relaxed)
               << L", backend_protocol_failures=" << backend.stats.protocolFailures.load(std::memory_order_relaxed)
               << std::endl;

    const bool sessionClosed = RemoveAllManagedDisks(&backend, true);
    if (!sessionClosed) {
        CloseHandle(backend.control.file);
        backend.control.file = INVALID_HANDLE_VALUE;
    }

    for (auto& worker : workers) {
        worker.join();
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
