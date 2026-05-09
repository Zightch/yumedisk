#pragma once

#include <Windows.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "yumedisk_proto.h"

namespace clientbackend {

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

class BackendContext;

} // namespace clientbackend
