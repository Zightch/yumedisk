#include "session/ak_session.h"

#include "protocol/ak_protocol.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

typedef struct AK_SESSION_TRANSPORT_SNAPSHOT {
    HANDLE ControlFile;
    UINT64 SessionId;
    BOOLEAN TransportReady;
} AK_SESSION_TRANSPORT_SNAPSHOT;

static void AkSessionLog(
    const AK_SESSION* session,
    INT level,
    const char* format,
    ...)
{
    char buffer[512];
    va_list args;
    int written;

    if ((session == NULL) || (session->OpenParams.LogFn == NULL) || (format == NULL)) {
        return;
    }

    va_start(args, format);
    written = vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);

    if (written < 0) {
        return;
    }

    buffer[sizeof(buffer) - 1u] = '\0';
    session->OpenParams.LogFn(session->OpenParams.LogCtx, level, buffer);
}

static void AkSessionRecordCommandFailure(
    AK_SESSION* session,
    AK_STATUS status)
{
    AcquireSRWLockExclusive(&session->Lock);
    session->Stats.CommandFailures += 1ull;
    session->State.LastError = status;
    ReleaseSRWLockExclusive(&session->Lock);
}

static void AkSessionRecordProtocolFailure(
    AK_SESSION* session,
    AK_STATUS status)
{
    AcquireSRWLockExclusive(&session->Lock);
    session->Stats.ProtocolFailures += 1ull;
    session->State.LastError = status;
    ReleaseSRWLockExclusive(&session->Lock);
}

static void AkSessionSetBroken(
    AK_SESSION* session,
    AK_STATUS status)
{
    AK_STATUS queue_status;

    AcquireSRWLockExclusive(&session->Lock);
    session->State.Lifecycle = AkStateBroken;
    session->State.TransportReady = FALSE;
    session->State.HeartbeatRunning = FALSE;
    session->State.LastError = status;
    ReleaseSRWLockExclusive(&session->Lock);

    queue_status = AkEventQueuePushSessionBroken(session, status);
    if (queue_status != AK_STATUS_SUCCESS) {
        AkSessionLog(
            session,
            3,
            "AkSessionSetBroken: enqueue broken event failed status=0x%08lX",
            (unsigned long)queue_status);
    }
}

static void AkSessionSnapshotTransport(
    AK_SESSION* session,
    AK_SESSION_TRANSPORT_SNAPSHOT* out_snapshot)
{
    AcquireSRWLockShared(&session->Lock);
    out_snapshot->ControlFile = session->ControlFile;
    out_snapshot->SessionId = session->State.SessionId;
    out_snapshot->TransportReady = session->State.TransportReady;
    ReleaseSRWLockShared(&session->Lock);
}

static AK_STATUS AkSessionValidateProtocolInfo(
    AK_SESSION* session,
    const YUMEDISK_QUERY_INFO* info)
{
    if (info->ProtocolVersion != YUMEDISK_PROTOCOL_VERSION) {
        AkSessionLog(
            session,
            3,
            "AkOpen: protocol version mismatch expected=%lu actual=%lu",
            (unsigned long)YUMEDISK_PROTOCOL_VERSION,
            (unsigned long)info->ProtocolVersion);
        return AK_STATUS_NOT_SUPPORTED;
    }

    if ((info->Features & YumeDiskFeatureAppOwnedQueue) == 0u) {
        AkSessionLog(
            session,
            3,
            "AkOpen: adapter missing app-owned-queue feature flags=0x%08lX",
            (unsigned long)info->Features);
        return AK_STATUS_NOT_SUPPORTED;
    }

    return AK_STATUS_SUCCESS;
}

static DWORD WINAPI AkSessionHeartbeatThreadProc(
    LPVOID context)
{
    AK_SESSION* session;

    session = (AK_SESSION*)context;
    for (;;) {
        AK_SESSION_TRANSPORT_SNAPSHOT snapshot;
        DWORD wait_status;
        AK_STATUS status;

        wait_status = WaitForSingleObject(
            session->StopEvent,
            session->OpenParams.HeartbeatIntervalMs);
        if (wait_status == WAIT_OBJECT_0) {
            break;
        }

        if (wait_status != WAIT_TIMEOUT) {
            status = AkFromWin32Error(GetLastError());
            AkSessionRecordCommandFailure(session, status);
            AkSessionSetBroken(session, status);
            AkSessionLog(
                session,
                3,
                "AkHeartbeat: wait failed status=0x%08lX",
                (unsigned long)status);
            SetEvent(session->StopEvent);
            break;
        }

        AkSessionSnapshotTransport(session, &snapshot);
        if (!snapshot.TransportReady || (snapshot.ControlFile == NULL) ||
            (snapshot.ControlFile == INVALID_HANDLE_VALUE) || (snapshot.SessionId == 0ull)) {
            break;
        }

        status = AkProtocolSendHeartbeat(snapshot.ControlFile, snapshot.SessionId);
        if (status != AK_STATUS_SUCCESS) {
            AkSessionRecordCommandFailure(session, status);
            AkSessionSetBroken(session, status);
            AkSessionLog(
                session,
                3,
                "AkHeartbeat: send failed session=%llu status=0x%08lX",
                (unsigned long long)snapshot.SessionId,
                (unsigned long)status);
            SetEvent(session->StopEvent);
            break;
        }

        AcquireSRWLockExclusive(&session->Lock);
        session->Stats.HeartbeatSent += 1ull;
        ReleaseSRWLockExclusive(&session->Lock);
    }

    AcquireSRWLockExclusive(&session->Lock);
    session->State.HeartbeatRunning = FALSE;
    ReleaseSRWLockExclusive(&session->Lock);
    return 0u;
}

static void AkSessionDestroy(
    AK_SESSION* session)
{
    if (session == NULL) {
        return;
    }

    if (session->HeartbeatThread != NULL) {
        CloseHandle(session->HeartbeatThread);
        session->HeartbeatThread = NULL;
    }

    if (session->StopEvent != NULL) {
        CloseHandle(session->StopEvent);
        session->StopEvent = NULL;
    }

    AkEventQueueDestroy(session);

    if ((session->ControlFile != NULL) && (session->ControlFile != INVALID_HANDLE_VALUE)) {
        CloseHandle(session->ControlFile);
        session->ControlFile = NULL;
    }

    AkFree(session);
}

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
    UINT64 session_id;

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
    session->EventQueue.InitialCapacity = params->InitialEventQueueCapacity;
    InitializeSRWLock(&session->Lock);
    session->ControlFile = NULL;
    session->StopEvent = NULL;
    session->HeartbeatThread = NULL;
    session->State.Lifecycle = AkStateStarting;
    session->State.LastError = AK_STATUS_SUCCESS;
    session->State.HeartbeatRunning = FALSE;
    session->State.TransportReady = FALSE;
    session->State.DiskCount = 0u;
    session->State.SessionId = 0ull;

    (void)memset(&session->Stats, 0, sizeof(session->Stats));

    AkSessionLog(session, 1, "AkOpen: begin");

    status = AkEventQueueInitialize(session);
    if (status != AK_STATUS_SUCCESS) {
        AkSessionRecordCommandFailure(session, status);
        AkSessionLog(session, 3, "AkOpen: init event queue failed status=0x%08lX", (unsigned long)status);
        AkSessionDestroy(session);
        return status;
    }

    status = AkProtocolOpenControlDevice(&session->ControlFile);
    if (status != AK_STATUS_SUCCESS) {
        AkSessionRecordCommandFailure(session, status);
        AkSessionLog(session, 3, "AkOpen: open control device failed status=0x%08lX", (unsigned long)status);
        AkSessionDestroy(session);
        return status;
    }

    status = AkProtocolQueryInfo(session->ControlFile, &session->QueryInfo, NULL);
    if (status != AK_STATUS_SUCCESS) {
        AkSessionRecordCommandFailure(session, status);
        AkSessionLog(session, 3, "AkOpen: query info failed status=0x%08lX", (unsigned long)status);
        AkSessionDestroy(session);
        return status;
    }

    status = AkSessionValidateProtocolInfo(session, &session->QueryInfo);
    if (status != AK_STATUS_SUCCESS) {
        AkSessionRecordProtocolFailure(session, status);
        AkSessionDestroy(session);
        return status;
    }

    status = AkProtocolQuerySessionId(session->ControlFile, &session_id);
    if (status != AK_STATUS_SUCCESS) {
        AkSessionRecordCommandFailure(session, status);
        AkSessionLog(session, 3, "AkOpen: query session id failed status=0x%08lX", (unsigned long)status);
        AkSessionDestroy(session);
        return status;
    }

    session->StopEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (session->StopEvent == NULL) {
        status = AkFromWin32Error(GetLastError());
        AkSessionRecordCommandFailure(session, status);
        AkSessionLog(session, 3, "AkOpen: create stop event failed status=0x%08lX", (unsigned long)status);
        (void)AkProtocolRemoveAllDisks(session->ControlFile, session_id, YUMEDISK_SESSION_CLOSE_FLAG);
        AkSessionDestroy(session);
        return status;
    }

    status = AkProtocolSendHeartbeat(session->ControlFile, session_id);
    if (status != AK_STATUS_SUCCESS) {
        AkSessionRecordCommandFailure(session, status);
        AkSessionLog(session, 3, "AkOpen: initial heartbeat failed session=%llu status=0x%08lX",
            (unsigned long long)session_id,
            (unsigned long)status);
        (void)AkProtocolRemoveAllDisks(session->ControlFile, session_id, YUMEDISK_SESSION_CLOSE_FLAG);
        AkSessionDestroy(session);
        return status;
    }

    session->HeartbeatThread = CreateThread(
        NULL,
        0u,
        AkSessionHeartbeatThreadProc,
        session,
        0u,
        NULL);
    if (session->HeartbeatThread == NULL) {
        status = AkFromWin32Error(GetLastError());
        AkSessionRecordCommandFailure(session, status);
        AkSessionLog(session, 3, "AkOpen: create heartbeat thread failed status=0x%08lX", (unsigned long)status);
        (void)AkProtocolRemoveAllDisks(session->ControlFile, session_id, YUMEDISK_SESSION_CLOSE_FLAG);
        AkSessionDestroy(session);
        return status;
    }

    AcquireSRWLockExclusive(&session->Lock);
    session->Stats.HeartbeatSent = 1ull;
    session->State.SessionId = session_id;
    session->State.TransportReady = TRUE;
    session->State.HeartbeatRunning = TRUE;
    session->State.Lifecycle = AkStateRunning;
    session->State.LastError = AK_STATUS_SUCCESS;
    ReleaseSRWLockExclusive(&session->Lock);

    AkSessionLog(
        session,
        1,
        "AkOpen: ready session=%llu features=0x%08lX heartbeat_ms=%lu",
        (unsigned long long)session_id,
        (unsigned long)session->QueryInfo.Features,
        (unsigned long)session->OpenParams.HeartbeatIntervalMs);

    *out_session = session;
    return AK_STATUS_SUCCESS;
}

void AkSessionClose(AK_SESSION* session)
{
    AK_SESSION_TRANSPORT_SNAPSHOT snapshot;
    AK_STATUS status;

    if (session == NULL) {
        return;
    }

    AkSessionLog(session, 1, "AkClose: begin");

    AcquireSRWLockExclusive(&session->Lock);
    if (session->State.Lifecycle != AkStateClosed) {
        session->State.Lifecycle = AkStateClosing;
        session->State.LastError = AK_STATUS_SUCCESS;
    }
    ReleaseSRWLockExclusive(&session->Lock);

    if (session->StopEvent != NULL) {
        SetEvent(session->StopEvent);
    }

    if (session->HeartbeatThread != NULL) {
        (void)CancelSynchronousIo(session->HeartbeatThread);
        (void)WaitForSingleObject(session->HeartbeatThread, INFINITE);
    }

    AkSessionSnapshotTransport(session, &snapshot);
    if (snapshot.TransportReady && (snapshot.SessionId != 0ull) &&
        (snapshot.ControlFile != NULL) && (snapshot.ControlFile != INVALID_HANDLE_VALUE)) {
        status = AkProtocolRemoveAllDisks(
            snapshot.ControlFile,
            snapshot.SessionId,
            YUMEDISK_SESSION_CLOSE_FLAG);
        if (status != AK_STATUS_SUCCESS) {
            AkSessionRecordCommandFailure(session, status);
            AkSessionLog(
                session,
                3,
                "AkClose: close session command failed session=%llu status=0x%08lX",
                (unsigned long long)snapshot.SessionId,
                (unsigned long)status);
        }
    }

    AcquireSRWLockExclusive(&session->Lock);
    session->State.TransportReady = FALSE;
    session->State.HeartbeatRunning = FALSE;
    session->State.Lifecycle = AkStateClosed;
    ReleaseSRWLockExclusive(&session->Lock);

    AkSessionLog(session, 1, "AkClose: finish");
    AkSessionDestroy(session);
}

AK_STATUS AkSessionRemoveAllDisks(AK_SESSION* session)
{
    AK_SESSION_TRANSPORT_SNAPSHOT snapshot;
    AK_STATUS status;

    if (session == NULL) {
        return AK_STATUS_INVALID_PARAMETER;
    }

    AkSessionSnapshotTransport(session, &snapshot);
    if (!snapshot.TransportReady || (snapshot.SessionId == 0ull) ||
        (snapshot.ControlFile == NULL) || (snapshot.ControlFile == INVALID_HANDLE_VALUE)) {
        return AK_STATUS_DEVICE_NOT_READY;
    }

    status = AkProtocolRemoveAllDisks(snapshot.ControlFile, snapshot.SessionId, 0u);
    if (status != AK_STATUS_SUCCESS) {
        AkSessionRecordCommandFailure(session, status);
    }

    return status;
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
