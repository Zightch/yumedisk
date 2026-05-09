#pragma once

#include <string>

#include "backend/types/types.h"

namespace clientbackend {

size_t countStagedFragmentsLocked(const DiskRuntime* diskRuntime);

bool initializeManagedDiskMedia(
    DiskRuntime* diskRuntime,
    MediaMode requestedMode,
    std::wstring* outReason);

void cleanupManagedDiskMedia(DiskRuntime* diskRuntime);

AK_STATUS AK_CALL hostReadBytes(
    void* mediaCtx,
    const AK_READ_OP* op,
    void* outBuffer,
    UINT32* outDataLength);

AK_STATUS AK_CALL hostStageWrite(
    void* mediaCtx,
    const AK_WRITE_OP* op,
    const void* dataBuffer,
    UINT32 dataLength);

bool applyCommittedWrite(
    DiskRuntime* diskRuntime,
    UINT64 eventId);

void discardStagedWrite(
    DiskRuntime* diskRuntime,
    UINT64 eventId);

} // namespace clientbackend
