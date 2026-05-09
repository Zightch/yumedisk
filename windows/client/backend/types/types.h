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

#include "backend/StagingStore/StagingStore.h"
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
    denseMem,
    sparseMem,
    rawFile
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
    std::wstring rawFilePath;
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

struct DiskMetadata {
    ULONG targetId = 0;
    ULONG sectorSize = 0;
    uint64_t diskSizeBytes = 0;
    bool readOnly = false;
    MediaMode mode = MediaMode::autoSelect;
    std::wstring backingFilePath;
    DiskIdentity identity{};
};

struct DiskQueueConfig {
    size_t slotDepth = 0;
    size_t readWorkerCount = 0;
    size_t writeWorkerCount = 0;
};

struct DiskLifecycleState {
    AK_DISK* handle = nullptr;
};

struct DiskMediaState {
    mutable std::shared_mutex lock;
    std::mutex backingFileIoLock;
    std::vector<unsigned char> memory;
    HANDLE backingFile = INVALID_HANDLE_VALUE;
};

struct DiskRuntime;

struct BackendContext {
    AppConfig config;
    AK_SESSION* session = nullptr;
    HANDLE stopEvent = nullptr;
    std::atomic<bool> stop{false};
    std::mutex diskRuntimesLock;
    std::map<ULONG, std::shared_ptr<DiskRuntime>> diskRuntimes;
    mutable std::mutex logLock;
    std::vector<std::wstring> logLines;
    std::thread eventThread;
    AK_STATUS openStatus = AK_STATUS_SUCCESS;
    DWORD openWin32Error = ERROR_SUCCESS;
    bool openSucceeded = false;

    bool open();
    void close();

    std::wstring querySessionStateText() const;
    std::vector<std::wstring> snapshotLogLines() const;
    std::vector<ManagedDiskSnapshot> snapshotManagedDisks() const;
    bool queryBackendStats(
        BackendStatsSnapshot* outStats,
        std::wstring* outErrorText = nullptr) const;
    bool queryDebugSnapshot(
        DebugSnapshot* outSnapshot,
        std::wstring* outErrorText = nullptr) const;

    ULONG findFirstFreeTarget();
    bool createManagedDisk(
        const CreateDiskRequest& request,
        std::wstring* outErrorText = nullptr);
    bool removeManagedDisk(
        ULONG targetId,
        std::wstring* outErrorText = nullptr);
    bool removeAllManagedDisks(bool closing);

    void appendLog(const std::wstring& text);
};

struct DiskRuntime {
    BackendContext* context = nullptr;
    DiskMetadata metadata;
    DiskQueueConfig queueConfig;
    DiskLifecycleState lifecycle;
    DiskMediaState media;
    StagingStore staging;
};

} // namespace clientbackend
