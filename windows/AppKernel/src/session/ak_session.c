#include "session/ak_session.h"

#include "disk/ak_disk.h"
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

static void AkSessionFormatVersion(
    ULONG version_be,
    char* buffer,
    size_t buffer_size)
{
    if ((buffer == NULL) || (buffer_size == 0u)) {
        return;
    }

    (void)snprintf(
        buffer,
        buffer_size,
        "%lu.%lu.%lu.%lu",
        (unsigned long)YUMEDISK_VERSION_MAJOR(version_be),
        (unsigned long)YUMEDISK_VERSION_MINOR(version_be),
        (unsigned long)YUMEDISK_VERSION_PATCH(version_be),
        (unsigned long)YUMEDISK_VERSION_BUILD(version_be));
    buffer[buffer_size - 1u] = '\0';
}

static AK_STATUS AkSessionValidateComponentVersions(
    AK_SESSION* session,
    const YUMEDISK_KMDF_INFO* kmdf_info,
    const YUMEDISK_SCSI_INFO* scsi_info)
{
    char expected[32];
    char actual[32];

    if ((session == NULL) || (kmdf_info == NULL) || (scsi_info == NULL)) {
        return AK_STATUS_INVALID_PARAMETER;
    }

    if (kmdf_info->VersionBe != YUMEDISK_COMPONENT_VERSION_BE) {
        AkSessionFormatVersion(YUMEDISK_COMPONENT_VERSION_BE, expected, sizeof(expected));
        AkSessionFormatVersion(kmdf_info->VersionBe, actual, sizeof(actual));
        AkSessionLog(
            session,
            3,
            "AkOpen: KMDF version mismatch expected=%s actual=%s",
            expected,
            actual);
        return AK_STATUS_NOT_SUPPORTED;
    }

    if (scsi_info->VersionBe != YUMEDISK_COMPONENT_VERSION_BE) {
        AkSessionFormatVersion(YUMEDISK_COMPONENT_VERSION_BE, expected, sizeof(expected));
        AkSessionFormatVersion(scsi_info->VersionBe, actual, sizeof(actual));
        AkSessionLog(
            session,
            3,
            "AkOpen: SCSI version mismatch expected=%s actual=%s",
            expected,
            actual);
        return AK_STATUS_NOT_SUPPORTED;
    }

    if ((scsi_info->Features & YumeDiskFeatureAppOwnedQueue) == 0u) {
        AkSessionLog(
            session,
            3,
            "AkOpen: adapter missing app-owned-queue feature flags=0x%08lX",
            (unsigned long)scsi_info->Features);
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
    AK_DISK* disk;
    AK_DISK* next_disk;

    if (session == NULL) {
        return;
    }

    disk = session->DiskListHead;
    session->DiskListHead = NULL;
    while (disk != NULL) {
        next_disk = disk->SessionNext;
        disk->SessionNext = NULL;
        disk->RegisteredInSession = FALSE;
        AkDiskDestroyDetached(disk);
        disk = next_disk;
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

static BOOLEAN AkSessionDiskTargetExistsLocked(
    const AK_SESSION* session,
    UINT32 target_id)
{
    const AK_DISK* disk;

    disk = session->DiskListHead;
    while (disk != NULL) {
        if (disk->State.TargetId == target_id) {
            return TRUE;
        }

        disk = disk->SessionNext;
    }

    return FALSE;
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
    session->DiskListHead = NULL;
    session->NextDiskRuntimeId = 1ull;
    session->NextTxId = 1ull;
    session->State.Lifecycle = AkStateStarting;
    session->State.LastError = AK_STATUS_SUCCESS;
    session->State.HeartbeatRunning = FALSE;
    session->State.TransportReady = FALSE;
    session->State.DiskCount = 0u;
    session->State.SessionId = 0ull;
    session->State.AppKernelVersionBe = AK_VERSION_BE;
    session->State.KmdfVersionBe = 0u;
    session->State.ScsiVersionBe = 0u;

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

    status = AkProtocolQueryKmdfInfo(session->ControlFile, &session->KmdfInfo, NULL);
    if (status != AK_STATUS_SUCCESS) {
        AkSessionRecordCommandFailure(session, status);
        AkSessionLog(session, 3, "AkOpen: query KMDF info failed status=0x%08lX", (unsigned long)status);
        AkSessionDestroy(session);
        return status;
    }

    status = AkProtocolQueryScsiInfo(session->ControlFile, &session->ScsiInfo);
    if (status != AK_STATUS_SUCCESS) {
        AkSessionRecordCommandFailure(session, status);
        AkSessionLog(session, 3, "AkOpen: query SCSI info failed status=0x%08lX", (unsigned long)status);
        AkSessionDestroy(session);
        return status;
    }

    status = AkSessionValidateComponentVersions(session, &session->KmdfInfo, &session->ScsiInfo);
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
    session->State.AppKernelVersionBe = AK_VERSION_BE;
    session->State.KmdfVersionBe = session->KmdfInfo.VersionBe;
    session->State.ScsiVersionBe = session->ScsiInfo.VersionBe;
    session->State.LastError = AK_STATUS_SUCCESS;
    ReleaseSRWLockExclusive(&session->Lock);

    AkSessionLog(
        session,
        1,
        "AkOpen: ready session=%llu kmdf=0x%08lX scsi=0x%08lX features=0x%08lX heartbeat_ms=%lu",
        (unsigned long long)session_id,
        (unsigned long)session->KmdfInfo.VersionBe,
        (unsigned long)session->ScsiInfo.VersionBe,
        (unsigned long)session->ScsiInfo.Features,
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

AK_STATUS AkSessionAcquireTransport(
    AK_SESSION* session,
    HANDLE* out_control_file,
    UINT64* out_session_id)
{
    if ((session == NULL) || (out_control_file == NULL) || (out_session_id == NULL)) {
        return AK_STATUS_INVALID_PARAMETER;
    }

    AcquireSRWLockShared(&session->Lock);
    if (!session->State.TransportReady || (session->ControlFile == NULL) ||
        (session->ControlFile == INVALID_HANDLE_VALUE) || (session->State.SessionId == 0ull)) {
        ReleaseSRWLockShared(&session->Lock);
        return AK_STATUS_DEVICE_NOT_READY;
    }

    *out_control_file = session->ControlFile;
    *out_session_id = session->State.SessionId;
    ReleaseSRWLockShared(&session->Lock);
    return AK_STATUS_SUCCESS;
}

UINT64 AkSessionAllocateTxId(
    AK_SESSION* session)
{
    UINT64 tx_id;

    if (session == NULL) {
        return 0ull;
    }

    AcquireSRWLockExclusive(&session->Lock);
    tx_id = session->NextTxId;
    session->NextTxId += 1ull;
    if (session->NextTxId == 0ull) {
        session->NextTxId = 1ull;
    }
    ReleaseSRWLockExclusive(&session->Lock);

    if (tx_id == 0ull) {
        return 1ull;
    }

    return tx_id;
}

AK_STATUS AkSessionRegisterDisk(
    AK_SESSION* session,
    AK_DISK* disk,
    UINT64* out_runtime_id)
{
    UINT64 runtime_id;

    if ((session == NULL) || (disk == NULL) || (out_runtime_id == NULL)) {
        return AK_STATUS_INVALID_PARAMETER;
    }

    AcquireSRWLockExclusive(&session->Lock);

    if (session->State.Lifecycle != AkStateRunning) {
        ReleaseSRWLockExclusive(&session->Lock);
        return AK_STATUS_DEVICE_NOT_READY;
    }

    if (AkSessionDiskTargetExistsLocked(session, disk->Params.TargetId)) {
        ReleaseSRWLockExclusive(&session->Lock);
        return AK_STATUS_ALREADY_EXISTS;
    }

    runtime_id = session->NextDiskRuntimeId;
    session->NextDiskRuntimeId += 1ull;

    disk->SessionNext = session->DiskListHead;
    session->DiskListHead = disk;
    disk->RegisteredInSession = TRUE;
    disk->State.DiskRuntimeId = runtime_id;
    disk->State.TargetId = disk->Params.TargetId;
    session->State.DiskCount += 1u;

    ReleaseSRWLockExclusive(&session->Lock);

    *out_runtime_id = runtime_id;
    return AK_STATUS_SUCCESS;
}

void AkSessionUnregisterDisk(
    AK_SESSION* session,
    AK_DISK* disk)
{
    AK_DISK** link;

    if ((session == NULL) || (disk == NULL)) {
        return;
    }

    AcquireSRWLockExclusive(&session->Lock);

    link = &session->DiskListHead;
    while (*link != NULL) {
        if (*link == disk) {
            *link = disk->SessionNext;
            disk->SessionNext = NULL;
            if (session->State.DiskCount > 0u) {
                session->State.DiskCount -= 1u;
            }
            break;
        }

        link = &(*link)->SessionNext;
    }

    disk->RegisteredInSession = FALSE;
    ReleaseSRWLockExclusive(&session->Lock);
}
