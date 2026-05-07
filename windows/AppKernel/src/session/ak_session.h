#pragma once

#include "common/ak_internal.h"
#include "event/ak_event.h"

AK_STATUS AkSessionOpen(
    const AK_OPEN_PARAMS* params,
    AK_SESSION** out_session);

void AkSessionClose(
    AK_SESSION* session);

AK_STATUS AkSessionRemoveAllDisks(
    AK_SESSION* session);

AK_STATUS AkSessionQueryState(
    AK_SESSION* session,
    AK_SESSION_STATE* out_state);

AK_STATUS AkSessionQueryStats(
    AK_SESSION* session,
    AK_SESSION_STATS* out_stats);

AK_STATUS AkSessionAcquireTransport(
    AK_SESSION* session,
    HANDLE* out_control_file,
    UINT64* out_session_id);

UINT64 AkSessionAllocateTxId(
    AK_SESSION* session);

AK_STATUS AkSessionRegisterDisk(
    AK_SESSION* session,
    AK_DISK* disk,
    UINT64* out_runtime_id);

void AkSessionUnregisterDisk(
    AK_SESSION* session,
    AK_DISK* disk);
