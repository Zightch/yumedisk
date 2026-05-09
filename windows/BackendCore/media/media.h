#pragma once

#include <memory>
#include <string>

#include "types/types.h"

namespace clientbackend {

bool initializeManagedDiskMedia(
    DiskRuntime* diskRuntime,
    MediaKind mediaKind,
    std::wstring* outReason);

void cleanupManagedDiskMedia(DiskRuntime* diskRuntime);

bool writeMediaRangeLocked(
    DiskRuntime* diskRuntime,
    UINT64 offset,
    const void* buffer,
    UINT32 length);

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

} // namespace clientbackend
