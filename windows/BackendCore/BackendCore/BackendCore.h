#pragma once

#include <Windows.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "yumedisk_proto.h"

#if defined(_WIN32)
#  if defined(BACKENDCORE_BUILD_SHARED)
#    define BACKENDCORE_API __declspec(dllexport)
#  else
#    define BACKENDCORE_API __declspec(dllimport)
#  endif
#else
#  define BACKENDCORE_API
#endif

namespace BackendCore {

inline constexpr ULONG defaultSectorSize = 4096;
inline constexpr UINT32 defaultQueueDepth = 32;
inline constexpr UINT32 defaultWriteSlotBytes = 1024 * 1024;
inline constexpr UINT16 defaultReadWorkerCount = 4;
inline constexpr UINT16 defaultWriteWorkerCount = 2;
inline constexpr UINT32 defaultAckBatchMaxRanges = defaultQueueDepth;
inline constexpr DWORD eventWaitPollMs = 100;
inline constexpr UINT32 defaultHeartbeatIntervalMs = 1000;
inline constexpr UINT32 defaultInitialEventQueueCapacity = 1024;
inline constexpr size_t maxBufferedLogLines = 256;

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

struct ManagedDiskSnapshot {
    ULONG targetId = 0;
    uint64_t diskSizeBytes = 0;
    ULONG sectorSize = 0;
    bool readOnly = false;
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

class BACKENDCORE_API Media {
public:
    virtual ~Media() = default;

    virtual bool readLocked(
        UINT64 offset,
        void* buffer,
        UINT32 length) = 0;

    virtual bool writeLocked(
        UINT64 offset,
        const void* buffer,
        UINT32 length) = 0;

    virtual uint64_t sizeBytes() const = 0;
};

class BACKENDCORE_API BackendContext final {
public:
    BackendContext();
    ~BackendContext();

    BackendContext(const BackendContext&) = delete;
    BackendContext& operator=(const BackendContext&) = delete;

    void setSessionConfig(const SessionConfig& sessionConfig);
    SessionConfig sessionConfig() const;

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
        DiskConfig diskConfig,
        std::unique_ptr<Media> media,
        std::wstring* outErrorText = nullptr);
    bool removeManagedDisk(
        ULONG targetId,
        std::wstring* outErrorText = nullptr);
    bool removeAllManagedDisks(bool closing);

    void appendLog(const std::wstring& text);

private:
    struct Impl;
    friend struct RuntimeAccess;

    std::unique_ptr<Impl> impl;
};

} // namespace BackendCore
