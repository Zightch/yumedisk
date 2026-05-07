#pragma once

#include <string>

#include "types.h"

namespace testapp {

size_t CountStagedFragmentsLocked(const ManagedDisk* disk);

bool InitializeManagedDiskMedia(
    ManagedDisk* disk,
    MediaMode requested_mode,
    std::wstring* out_reason);

void CleanupManagedDiskMedia(ManagedDisk* disk);

AK_STATUS AK_CALL HostReadBytes(
    void* media_ctx,
    const AK_READ_OP* op,
    void* out_buffer,
    UINT32* out_data_length);

AK_STATUS AK_CALL HostStageWrite(
    void* media_ctx,
    const AK_WRITE_OP* op,
    const void* data_buffer,
    UINT32 data_length);

bool ApplyCommittedWrite(
    ManagedDisk* disk,
    UINT64 event_id);

void DiscardStagedWrite(
    ManagedDisk* disk,
    UINT64 event_id);

} // namespace testapp
