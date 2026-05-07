#pragma once

#include "common/ak_internal.h"

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
