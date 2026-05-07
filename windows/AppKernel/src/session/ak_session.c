#include "session/ak_session.h"

#include <string.h>

static AK_STATUS AkValidateOpenParams(const AK_OPEN_PARAMS* params)
{
    if (params == NULL) {
        return AK_STATUS_INVALID_PARAMETER;
    }

    if (params->HeartbeatIntervalMs == 0u) {
        return AK_STATUS_INVALID_PARAMETER;
    }

    if (params->InitialEventQueueCapacity == 0u) {
        return AK_STATUS_INVALID_PARAMETER;
    }

    return AK_STATUS_SUCCESS;
}

AK_STATUS AkSessionOpen(
    const AK_OPEN_PARAMS* params,
    AK_SESSION** out_session)
{
    AK_SESSION* session;
    AK_STATUS status;

    if (out_session == NULL) {
        return AK_STATUS_INVALID_PARAMETER;
    }

    *out_session = NULL;

    status = AkValidateOpenParams(params);
    if (status != AK_STATUS_SUCCESS) {
        return status;
    }

    session = (AK_SESSION*)AkAllocZero(sizeof(*session));
    if (session == NULL) {
        return AK_STATUS_INSUFFICIENT_RESOURCES;
    }

    session->OpenParams = *params;
    session->State.Lifecycle = AkStateInit;
    session->State.LastError = AK_STATUS_SUCCESS;
    session->State.HeartbeatRunning = FALSE;
    session->State.TransportReady = FALSE;
    session->State.DiskCount = 0u;

    (void)memset(&session->Stats, 0, sizeof(session->Stats));

    *out_session = session;
    return AK_STATUS_SUCCESS;
}

void AkSessionClose(AK_SESSION* session)
{
    if (session == NULL) {
        return;
    }

    session->State.Lifecycle = AkStateClosed;
    AkFree(session);
}

AK_STATUS AkSessionRemoveAllDisks(AK_SESSION* session)
{
    if (session == NULL) {
        return AK_STATUS_INVALID_PARAMETER;
    }

    (void)session;
    return AK_STATUS_NOT_SUPPORTED;
}

AK_STATUS AkSessionQueryState(
    AK_SESSION* session,
    AK_SESSION_STATE* out_state)
{
    if ((session == NULL) || (out_state == NULL)) {
        return AK_STATUS_INVALID_PARAMETER;
    }

    *out_state = session->State;
    return AK_STATUS_SUCCESS;
}

AK_STATUS AkSessionQueryStats(
    AK_SESSION* session,
    AK_SESSION_STATS* out_stats)
{
    if ((session == NULL) || (out_stats == NULL)) {
        return AK_STATUS_INVALID_PARAMETER;
    }

    *out_stats = session->Stats;
    return AK_STATUS_SUCCESS;
}
