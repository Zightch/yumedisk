#pragma once

#include <Windows.h>

#include <cstdint>
#include <memory>
#include <shared_mutex>

#include "StagingStore/StagingStore.h"
#include "appkernel.h"
#include "media/Media/Media.h"
#include "scan.h"
#include "types/types.h"

namespace BackendCore {

using yumedisk::scan::DiskIdentity;

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

struct DiskRuntime {
    BackendContext* context = nullptr;
    DiskMetadata metadata;
    DiskQueueConfig queueConfig;
    DiskLifecycleState lifecycle;
    DiskMediaState media;
    StagingStore staging;
};

} // namespace BackendCore

