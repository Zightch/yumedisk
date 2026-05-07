#include "disk/ak_disk.h"

#include "protocol/ak_protocol.h"

AK_STATUS AkDiskCreate(
    AK_SESSION* session,
    const AK_DISK_PARAMS* params,
    const AK_MEDIA_OPS* media_ops,
    void* media_ctx,
    AK_DISK** out_disk)
{
    (void)session;
    (void)params;
    (void)media_ops;
    (void)media_ctx;
    (void)out_disk;
    return AkProtocolUnavailable();
}

AK_STATUS AkDiskRemove(AK_DISK* disk)
{
    (void)disk;
    return AkProtocolUnavailable();
}

AK_STATUS AkDiskQueryState(
    AK_DISK* disk,
    AK_DISK_STATE* out_state)
{
    (void)disk;
    (void)out_state;
    return AkProtocolUnavailable();
}

AK_STATUS AkDiskQueryStats(
    AK_DISK* disk,
    AK_DISK_STATS* out_stats)
{
    (void)disk;
    (void)out_stats;
    return AkProtocolUnavailable();
}
