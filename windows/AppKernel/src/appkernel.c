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

AK_STATUS AK_CALL AkWaitResponse(
    AK_SESSION* session,
    DWORD timeout_ms,
    AK_RESPONSE* out_response)
{
    return AkResponseWait(session, timeout_ms, out_response);
}

AK_STATUS AK_CALL AkPollResponse(
    AK_SESSION* session,
    AK_RESPONSE* out_response)
{
    return AkResponsePoll(session, out_response);
}

AK_STATUS AK_CALL AkWaitSessionNotice(
    AK_SESSION* session,
    DWORD timeout_ms,
    AK_SESSION_NOTICE* out_notice)
{
    return AkSessionNoticeWait(session, timeout_ms, out_notice);
}

AK_STATUS AK_CALL AkPollSessionNotice(
    AK_SESSION* session,
    AK_SESSION_NOTICE* out_notice)
{
    return AkSessionNoticePoll(session, out_notice);
}

AK_STATUS AK_CALL AkCreateDisk(
    AK_SESSION* session,
    const AK_DISK_PARAMS* params,
    const AK_DISK_OPS* disk_ops,
    void* disk_ctx,
    AK_DISK** out_disk)
{
    return AkDiskCreate(session, params, disk_ops, disk_ctx, out_disk);
}

AK_STATUS AK_CALL AkRemoveDisk(
    AK_DISK* disk)
{
    return AkDiskRemove(disk);
}

AK_STATUS AK_CALL AkDetachDisk(
    AK_DISK* disk)
{
    return AkDiskDetach(disk);
}

AK_STATUS AK_CALL AkNotifyDiskDataChanged(
    AK_DISK* disk)
{
    return AkDiskNotifyDataChanged(disk);
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
