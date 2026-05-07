#pragma once

#include <Windows.h>
#include <SetupAPI.h>
#include <WinIoCtl.h>

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
#include "yumedisk_proto.h"

namespace testapp {

constexpr ULONG kDefaultSectorSize = 4096;
constexpr size_t kDefaultQueueDepth = 32;
constexpr size_t kMaxSlotEngineQueueDepth = MAXIMUM_WAIT_OBJECTS / 2;
constexpr size_t kDefaultWriteSlotBytes = 1024 * 1024;
constexpr uint64_t kMaxDenseMediaBytes = 1024ull * 1024ull * 1024ull;
constexpr size_t kMaxReadWorkersPerDisk = 4;
constexpr size_t kMaxWriteWorkersPerDisk = 2;
constexpr size_t kReadSlotsPerWorkerTarget = 8;
constexpr size_t kWriteSlotsPerWorkerTarget = 16;
constexpr DWORD kDiskArrivalPollMs = 100;
constexpr DWORD kDiskArrivalTimeoutMs = 2000;
constexpr DWORD kEventWaitPollMs = 100;
constexpr UINT32 kHeartbeatIntervalMs = 1000;
constexpr UINT32 kInitialEventQueueCapacity = 1024;

enum class MediaMode {
    Auto,
    Dense,
    Sparse
};

enum class ParseResult {
    Ok,
    Help,
    Error
};

struct AppConfig {
    ULONG SectorSize = kDefaultSectorSize;
    size_t QueueDepth = kDefaultQueueDepth;
    size_t WriteSlotBytes = kDefaultWriteSlotBytes;
};

struct CreateDiskRequest {
    ULONG TargetId = YUMEDISK_MAX_TARGETS;
    uint64_t DiskSizeBytes = 0;
    MediaMode RequestedMode = MediaMode::Auto;
    bool ReadOnly = false;
};

struct DiskIdentity {
    std::wstring Path;
    std::wstring Vendor;
    std::wstring Product;
    uint64_t LengthBytes = 0;
    DWORD DeviceNumber = std::numeric_limits<DWORD>::max();
};

struct StagedFragment {
    UINT32 Seq = 0;
    UINT64 DiskOffsetBytes = 0;
    UINT64 Ordinal = 0;
    std::vector<unsigned char> Data;
};

struct StagedWriteRecord {
    UINT32 TotalSeq = 0;
    std::map<UINT32, StagedFragment> Fragments;
};

struct ManagedDisk;

struct BackendContext {
    AppConfig Config;
    AK_SESSION* Session = nullptr;
    HANDLE StopEvent = nullptr;
    std::atomic<bool> Stop{false};
    std::mutex DisksLock;
    std::map<ULONG, std::shared_ptr<ManagedDisk>> Disks;
    std::mutex LogLock;
    std::thread EventThread;
};

struct ManagedDisk {
    BackendContext* Backend = nullptr;
    AK_DISK* Handle = nullptr;
    ULONG TargetId = 0;
    ULONG SectorSize = 0;
    uint64_t DiskSizeBytes = 0;
    bool ReadOnly = false;
    size_t SlotDepth = 0;
    size_t ReadWorkerCount = 0;
    size_t WriteWorkerCount = 0;
    MediaMode Mode = MediaMode::Auto;
    DiskIdentity Identity{};
    mutable std::shared_mutex MediaLock;
    std::mutex SparseIoLock;
    std::vector<unsigned char> DenseMedium;
    HANDLE SparseFile = INVALID_HANDLE_VALUE;
    std::wstring SparseBackingPath;
    std::map<UINT64, StagedWriteRecord> StagedWrites;
    UINT64 NextStageOrdinal = 1;
};

} // namespace testapp
