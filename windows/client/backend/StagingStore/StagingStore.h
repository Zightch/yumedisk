#pragma once

#include <Windows.h>

#include <cstddef>
#include <cstdint>
#include <map>
#include <vector>

#include "appkernel.h"

namespace clientbackend {

struct DiskRuntime;

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

using MediaWriteRangeFn = bool(*)(
    DiskRuntime* diskRuntime,
    UINT64 offset,
    const void* buffer,
    UINT32 length);

class StagingStore final {
public:
    size_t countFragmentsLocked() const;

    AK_STATUS stageWriteLocked(
        const AK_WRITE_OP* op,
        const void* dataBuffer,
        UINT32 dataLength,
        uint64_t diskSizeBytes);

    void overlayReadLocked(
        UINT64 requestBegin,
        unsigned char* buffer,
        UINT32 dataLength) const;

    bool commitLocked(
        UINT64 eventId,
        uint64_t diskSizeBytes,
        DiskRuntime* diskRuntime,
        MediaWriteRangeFn writeRangeFn);

    void rejectLocked(UINT64 eventId);
    void clearLocked();

private:
    std::map<UINT64, StagedWriteRecord> writes;
    UINT64 nextOrdinal = 1;
};

bool commitDiskRuntimeStaging(
    DiskRuntime* diskRuntime,
    UINT64 eventId);

void rejectDiskRuntimeStaging(
    DiskRuntime* diskRuntime,
    UINT64 eventId);

} // namespace clientbackend
