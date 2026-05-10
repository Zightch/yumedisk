#pragma once

#include <memory>
#include <string>

#include "BackendCore.h"
#include "appkernel.h"

namespace BackendCore {

struct DiskRuntime;

bool adoptManagedDiskMedia(
    DiskRuntime* diskRuntime,
    std::unique_ptr<Media> media,
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

} // namespace BackendCore

