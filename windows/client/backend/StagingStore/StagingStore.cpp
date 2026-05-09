#include "backend/StagingStore/StagingStore.h"

#include <algorithm>
#include <cstring>
#include <shared_mutex>

#include "backend/media/media.h"
#include "backend/types/types.h"

namespace clientbackend {

namespace {

struct OverlaySlice {
    UINT64 ordinal = 0;
    size_t destOffset = 0;
    size_t sourceOffset = 0;
    size_t length = 0;
    const unsigned char* data = nullptr;
};

} // namespace

size_t StagingStore::countFragmentsLocked() const
{
    size_t count = 0;

    for (const auto& entry : writes) {
        count += entry.second.fragments.size();
    }

    return count;
}

AK_STATUS StagingStore::stageWriteLocked(
    const AK_WRITE_OP* op,
    const void* dataBuffer,
    UINT32 dataLength,
    uint64_t diskSizeBytes)
{
    UINT64 writeBegin;
    UINT64 writeEnd;
    StagedWriteRecord* record;
    StagedFragment fragment;

    if (op == nullptr) {
        return AK_STATUS_INVALID_PARAMETER;
    }

    if (dataLength != op->DataLength) {
        return AK_STATUS_INVALID_PARAMETER;
    }

    writeBegin = op->OffsetBytes;
    writeEnd = writeBegin + (UINT64)dataLength;
    if ((writeEnd < writeBegin) || (writeEnd > diskSizeBytes)) {
        return AK_STATUS_INVALID_PARAMETER;
    }

    record = &writes[op->EventId];
    if ((record->totalSeq != 0) && (record->totalSeq != op->TotalSeq)) {
        return AK_STATUS_INVALID_PARAMETER;
    }

    record->totalSeq = op->TotalSeq;
    fragment.seq = op->Seq;
    fragment.diskOffsetBytes = op->OffsetBytes;
    fragment.ordinal = nextOrdinal;
    nextOrdinal += 1;
    fragment.data.resize(dataLength, 0u);
    if ((dataLength != 0) && (dataBuffer != nullptr)) {
        (void)memcpy(fragment.data.data(), dataBuffer, dataLength);
    }

    record->fragments[op->Seq] = std::move(fragment);
    return AK_STATUS_SUCCESS;
}

void StagingStore::overlayReadLocked(
    UINT64 requestBegin,
    unsigned char* buffer,
    UINT32 dataLength) const
{
    std::vector<OverlaySlice> overlays;
    const UINT64 requestEnd = requestBegin + (UINT64)dataLength;

    if ((buffer == nullptr) || (dataLength == 0)) {
        return;
    }

    for (const auto& stagedEntry : writes) {
        for (const auto& fragmentEntry : stagedEntry.second.fragments) {
            const StagedFragment& fragment = fragmentEntry.second;
            const UINT64 fragmentBegin = fragment.diskOffsetBytes;
            const UINT64 fragmentEnd = fragmentBegin + (UINT64)fragment.data.size();
            UINT64 overlapBegin;
            UINT64 overlapEnd;
            OverlaySlice slice;

            if ((fragmentEnd <= requestBegin) || (fragmentBegin >= requestEnd)) {
                continue;
            }

            overlapBegin = std::max(fragmentBegin, requestBegin);
            overlapEnd = std::min(fragmentEnd, requestEnd);
            if (overlapEnd <= overlapBegin) {
                continue;
            }

            slice.ordinal = fragment.ordinal;
            slice.destOffset = (size_t)(overlapBegin - requestBegin);
            slice.sourceOffset = (size_t)(overlapBegin - fragmentBegin);
            slice.length = (size_t)(overlapEnd - overlapBegin);
            slice.data = fragment.data.data();
            overlays.push_back(slice);
        }
    }

    std::sort(
        overlays.begin(),
        overlays.end(),
        [](const OverlaySlice& left, const OverlaySlice& right) {
            return left.ordinal < right.ordinal;
        });

    for (const auto& slice : overlays) {
        (void)memcpy(
            buffer + slice.destOffset,
            slice.data + slice.sourceOffset,
            slice.length);
    }
}

bool StagingStore::commitLocked(
    UINT64 eventId,
    uint64_t diskSizeBytes,
    DiskRuntime* diskRuntime,
    MediaWriteRangeFn writeRangeFn)
{
    const auto it = writes.find(eventId);

    if (it == writes.end()) {
        return true;
    }
    if ((diskRuntime == nullptr) || (writeRangeFn == nullptr)) {
        return false;
    }

    for (const auto& fragmentEntry : it->second.fragments) {
        const StagedFragment& fragment = fragmentEntry.second;
        const UINT64 endOffset = fragment.diskOffsetBytes + (UINT64)fragment.data.size();

        if ((endOffset < fragment.diskOffsetBytes) || (endOffset > diskSizeBytes)) {
            return false;
        }

        if (!fragment.data.empty() &&
            !writeRangeFn(
                diskRuntime,
                fragment.diskOffsetBytes,
                fragment.data.data(),
                (UINT32)fragment.data.size())) {
            return false;
        }
    }

    writes.erase(it);
    return true;
}

void StagingStore::rejectLocked(
    UINT64 eventId)
{
    writes.erase(eventId);
}

void StagingStore::clearLocked()
{
    writes.clear();
    nextOrdinal = 1;
}

bool commitDiskRuntimeStaging(
    DiskRuntime* diskRuntime,
    UINT64 eventId)
{
    std::unique_lock<std::shared_mutex> guard(diskRuntime->media.lock);

    return diskRuntime->staging.commitLocked(
        eventId,
        diskRuntime->metadata.diskSizeBytes,
        diskRuntime,
        writeMediaRangeLocked);
}

void rejectDiskRuntimeStaging(
    DiskRuntime* diskRuntime,
    UINT64 eventId)
{
    std::unique_lock<std::shared_mutex> guard(diskRuntime->media.lock);
    diskRuntime->staging.rejectLocked(eventId);
}

} // namespace clientbackend
