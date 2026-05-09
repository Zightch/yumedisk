#pragma once

#include <Windows.h>

#include <atomic>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <thread>
#include <vector>

#include "media/Media/Media.h"
#include "StagingStore/StagingStore.h"
#include "appkernel.h"
#include "scan.h"
#include "yumedisk_proto.h"

namespace clientbackend {

using yumedisk::scan::DiskIdentity;

inline constexpr ULONG defaultSectorSize = 4096;
inline constexpr UINT32 defaultQueueDepth = 32;
inline constexpr UINT32 defaultWriteSlotBytes = 1024 * 1024;
inline constexpr UINT16 defaultReadWorkerCount = 4;
inline constexpr UINT16 defaultWriteWorkerCount = 2;
inline constexpr UINT32 defaultAckBatchMaxRanges = defaultQueueDepth;
inline constexpr DWORD diskArrivalPollMs = 100;
inline constexpr DWORD diskArrivalTimeoutMs = 2000;
inline constexpr DWORD eventWaitPollMs = 100;
inline constexpr UINT32 defaultHeartbeatIntervalMs = 1000;
inline constexpr UINT32 defaultInitialEventQueueCapacity = 1024;
inline constexpr size_t maxBufferedLogLines = 256;

enum class MediaKind {
    unknown,
    denseMem,
    sparseMem,
    rawFile
};

struct SessionConfig {
    UINT32 heartbeatIntervalMs = defaultHeartbeatIntervalMs;
    UINT32 initialEventQueueCapacity = defaultInitialEventQueueCapacity;
};

struct DiskConfig {
    ULONG targetId = YUMEDISK_MAX_TARGETS;
    ULONG sectorSize = defaultSectorSize;
    uint64_t diskSizeBytes = 0;
    UINT32 queueDepth = defaultQueueDepth;
    UINT32 writeSlotBytes = defaultWriteSlotBytes;
    UINT16 readWorkerCount = defaultReadWorkerCount;
    UINT16 writeWorkerCount = defaultWriteWorkerCount;
    UINT32 ackBatchMaxRanges = defaultAckBatchMaxRanges;
    bool readOnly = false;
};

struct CreateDiskRequest {
    DiskConfig diskConfig;
    MediaKind mediaKind = MediaKind::unknown;
};

struct ManagedDiskSnapshot {
    ULONG targetId = 0;
    uint64_t diskSizeBytes = 0;
    ULONG sectorSize = 0;
    bool readOnly = false;
    MediaKind mediaKind = MediaKind::unknown;
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
    MediaKind mediaKind = MediaKind::unknown;
    DiskIdentity identity{};
};

struct DiskQueueConfig {
    UINT32 queueDepth = 0;
    UINT32 writeSlotBytes = 0;
    UINT16 readWorkerCount = 0;
    UINT16 writeWorkerCount = 0;
    UINT32 ackBatchMaxRanges = 0;
};

struct DiskLifecycleState {
    AK_DISK* handle = nullptr;
};

struct DiskMediaState {
    mutable std::shared_mutex lock;
    std::unique_ptr<Media> instance;
};

struct DiskRuntime;

struct BackendContext {
    SessionConfig sessionConfig;
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
