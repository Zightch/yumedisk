#include "appkernel.h"

#include "disk/ak_disk.h"
#include "event/ak_event.h"
#include "protocol/ak_protocol.h"
#include "session/ak_session.h"

AK_STATUS AK_CALL AkOpen(
    const AK_OPEN_PARAMS* params,
    AK_SESSION** out_session)
{
    return AkSessionOpen(params, out_session);
}

VOID AK_CALL AkClose(
    AK_SESSION* session)
{
    AkSessionClose(session);
}

AK_STATUS AK_CALL AkRemoveAllDisks(
    AK_SESSION* session)
{
    return AkSessionRemoveAllDisks(session);
}

AK_STATUS AK_CALL AkQuerySessionState(
    AK_SESSION* session,
    AK_SESSION_STATE* out_state)
{
    return AkSessionQueryState(session, out_state);
}

AK_STATUS AK_CALL AkQuerySessionStats(
    AK_SESSION* session,
    AK_SESSION_STATS* out_stats)
{
    return AkSessionQueryStats(session, out_stats);
}

AK_STATUS AK_CALL AkWaitEvent(
    AK_SESSION* session,
    DWORD timeout_ms,
    AK_EVENT* out_event)
{
    return AkEventWait(session, timeout_ms, out_event);
}

AK_STATUS AK_CALL AkPollEvent(
    AK_SESSION* session,
    AK_EVENT* out_event)
{
    return AkEventPoll(session, out_event);
}

AK_STATUS AK_CALL AkCreateDisk(
    AK_SESSION* session,
    const AK_DISK_PARAMS* params,
    const AK_MEDIA_OPS* media_ops,
    void* media_ctx,
    AK_DISK** out_disk)
{
    return AkDiskCreate(session, params, media_ops, media_ctx, out_disk);
}

AK_STATUS AK_CALL AkRemoveDisk(
    AK_DISK* disk)
{
    return AkDiskRemove(disk);
}

AK_STATUS AK_CALL AkQueryDiskState(
    AK_DISK* disk,
    AK_DISK_STATE* out_state)
{
    return AkDiskQueryState(disk, out_state);
}

AK_STATUS AK_CALL AkQueryDiskStats(
    AK_DISK* disk,
    AK_DISK_STATS* out_stats)
{
    return AkDiskQueryStats(disk, out_stats);
}
