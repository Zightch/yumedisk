#pragma once

#include <Windows.h>

#include <atomic>
#include <cstdint>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <thread>
#include <vector>

#include "appkernel.h"
#include "scan.h"
#include "yumedisk_proto.h"

namespace clientbackend {

using yumedisk::scan::DiskIdentity;

inline constexpr ULONG defaultSectorSize = 4096;
inline constexpr size_t defaultQueueDepth = 32;
inline constexpr size_t maxSlotEngineQueueDepth = MAXIMUM_WAIT_OBJECTS / 2;
inline constexpr size_t defaultWriteSlotBytes = 1024 * 1024;
inline constexpr uint64_t maxDenseMediaBytes = 1024ull * 1024ull * 1024ull;
inline constexpr size_t maxReadWorkersPerDisk = 4;
inline constexpr size_t maxWriteWorkersPerDisk = 2;
inline constexpr size_t readSlotsPerWorkerTarget = 8;
inline constexpr size_t writeSlotsPerWorkerTarget = 16;
inline constexpr DWORD diskArrivalPollMs = 100;
inline constexpr DWORD diskArrivalTimeoutMs = 2000;
inline constexpr DWORD eventWaitPollMs = 100;
inline constexpr UINT32 heartbeatIntervalMs = 1000;
inline constexpr UINT32 initialEventQueueCapacity = 1024;
inline constexpr size_t maxBufferedLogLines = 256;

enum class MediaMode {
    autoSelect,
    dense,
    sparse
};

struct AppConfig {
    ULONG sectorSize = defaultSectorSize;
    size_t queueDepth = defaultQueueDepth;
    size_t writeSlotBytes = defaultWriteSlotBytes;
};

struct CreateDiskRequest {
    ULONG targetId = YUMEDISK_MAX_TARGETS;
    uint64_t diskSizeBytes = 0;
    MediaMode requestedMode = MediaMode::autoSelect;
    bool readOnly = false;
};

struct ManagedDiskSnapshot {
    ULONG targetId = 0;
    uint64_t diskSizeBytes = 0;
    ULONG sectorSize = 0;
    bool readOnly = false;
    MediaMode mode = MediaMode::autoSelect;
    std::wstring visiblePath;
    std::wstring physicalDrivePath;
    std::wstring lifecycleText;
    bool online = false;
};

struct BackendStatsSnapshot {
    UINT64 heartbeatSent = 0;
    UINT64 commandFailures = 0;
    UINT64 protocolFailures = 0;
    UINT64 eventsQueued = 0;
    UINT64 eventsDropped = 0;
    UINT64 diskCount = 0;
};

struct DebugSnapshot {
    std::wstring sessionStateText;
    BackendStatsSnapshot stats;
    std::vector<ManagedDiskSnapshot> disks;
};

struct StagedFragment {
    UINT32 seq = 0;
    UINT64 diskOffsetBytes = 0;
    UINT64 ordinal = 0;
    std::vector<unsigned char> data;
};

struct StagedWriteRecord {
    UINT32 totalSeq = 0;
    std::map<UINT32, StagedFragment> fragments;
};

struct ManagedDisk;

struct BackendContext {
    AppConfig config;
    AK_SESSION* session = nullptr;
    HANDLE stopEvent = nullptr;
    std::atomic<bool> stop{false};
    std::mutex disksLock;
    std::map<ULONG, std::shared_ptr<ManagedDisk>> disks;
    mutable std::mutex logLock;
    std::vector<std::wstring> logLines;
    std::thread eventThread;
    AK_STATUS openStatus = AK_STATUS_SUCCESS;
    DWORD openWin32Error = ERROR_SUCCESS;
    bool openSucceeded = false;
};

struct ManagedDisk {
    BackendContext* backend = nullptr;
    AK_DISK* handle = nullptr;
    ULONG targetId = 0;
    ULONG sectorSize = 0;
    uint64_t diskSizeBytes = 0;
    bool readOnly = false;
    size_t slotDepth = 0;
    size_t readWorkerCount = 0;
    size_t writeWorkerCount = 0;
    MediaMode mode = MediaMode::autoSelect;
    DiskIdentity identity{};
    mutable std::shared_mutex mediaLock;
    std::mutex sparseIoLock;
    std::vector<unsigned char> denseMedium;
    HANDLE sparseFile = INVALID_HANDLE_VALUE;
    std::wstring sparseBackingPath;
    std::map<UINT64, StagedWriteRecord> stagedWrites;
    UINT64 nextStageOrdinal = 1;
};

} // namespace clientbackend
