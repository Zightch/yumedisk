#pragma once

#include <Windows.h>

#include <cstdint>
#include <map>
#include <memory>
#include <string>

#include "BackendCore.h"
#include "scan.h"

namespace testapp {

using YumeDisk::Scan::DiskIdentity;

constexpr ULONG kDefaultSectorSize = BackendCore::defaultSectorSize;
constexpr size_t kDefaultQueueDepth = BackendCore::defaultQueueDepth;
constexpr size_t kMaxSlotEngineQueueDepth = MAXIMUM_WAIT_OBJECTS / 2;
constexpr size_t kDefaultWriteSlotBytes = BackendCore::defaultWriteSlotBytes;
constexpr uint64_t kMaxDenseMediaBytes = 1024ull * 1024ull * 1024ull;
constexpr size_t kMaxReadWorkersPerDisk = 4;
constexpr size_t kMaxWriteWorkersPerDisk = 2;
constexpr size_t kReadSlotsPerWorkerTarget = 8;
constexpr size_t kWriteSlotsPerWorkerTarget = 16;
constexpr DWORD kDiskArrivalPollMs = BackendCore::diskArrivalPollMs;
constexpr DWORD kDiskArrivalTimeoutMs = BackendCore::diskArrivalTimeoutMs;

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

struct ManagedDiskLocalState {
    MediaMode Mode = MediaMode::Auto;
    std::wstring BackingDescription;
    UINT16 ReadWorkerCount = 0;
    UINT16 WriteWorkerCount = 0;
    UINT32 QueueDepth = 0;
};

struct CliContext {
    AppConfig Config;
    BackendCore::BackendContext Backend;
    std::map<ULONG, ManagedDiskLocalState> Disks;
};

} // namespace testapp
