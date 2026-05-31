#pragma once

#include "common/ak_internal.h"

AK_STATUS AkDiskCreate(
    AK_SESSION* session,
    const AK_DISK_PARAMS* params,
    const AK_DISK_OPS* disk_ops,
    void* disk_ctx,
    AK_DISK** out_disk);

AK_STATUS AkDiskRemove(
    AK_DISK* disk);

AK_STATUS AkDiskNotifyDataChanged(
    AK_DISK* disk);

AK_STATUS AkDiskQueryState(
    AK_DISK* disk,
    AK_DISK_STATE* out_state);

AK_STATUS AkDiskQueryStats(
    AK_DISK* disk,
    AK_DISK_STATS* out_stats);

void AkDiskDestroyDetached(
    AK_DISK* disk);
