#include "disk/ak_disk.h"

#include "event/ak_event.h"
#include "protocol/ak_protocol.h"
#include "session/ak_session.h"

#include <string.h>

#define AK_DISK_WORKER_KIND_READ 1u
#define AK_DISK_WORKER_KIND_WRITE 2u
#define AK_DISK_WORKER_KIND_ACK 3u

static void AkDiskFreeWorkerArrays(
    AK_DISK* disk);

static void AkDiskStopWorkers(
    AK_DISK* disk);

static DWORD WINAPI AkDiskWorkerThreadProc(
    LPVOID context)
{
    AK_DISK_WORKER_CONTEXT* worker;
    AK_DISK* disk;

    worker = (AK_DISK_WORKER_CONTEXT*)context;
    if ((worker == NULL) || (worker->Disk == NULL)) {
        return 0u;
    }

    disk = worker->Disk;
    if (disk->StopEvent != NULL) {
        (void)WaitForSingleObject(disk->StopEvent, INFINITE);
    }

    return 0u;
}

static AK_STATUS AkDiskValidateParams(
    const AK_DISK_PARAMS* params,
    const AK_MEDIA_OPS* media_ops)
{
    if ((params == NULL) || (media_ops == NULL)) {
        return AK_STATUS_INVALID_PARAMETER;
    }

    if ((params->SectorSize == 0u) || (params->DiskSizeBytes == 0ull) ||
        ((params->DiskSizeBytes % params->SectorSize) != 0ull)) {
        return AK_STATUS_INVALID_PARAMETER;
    }

    if (params->QueueDepth == 0u) {
        return AK_STATUS_INVALID_PARAMETER;
    }

    if (params->WriteSlotBytes == 0u) {
        return AK_STATUS_INVALID_PARAMETER;
    }

    if ((params->ReadWorkerCount == 0u) || (params->WriteWorkerCount == 0u)) {
        return AK_STATUS_INVALID_PARAMETER;
    }

    if (params->AckBatchMaxRanges == 0u) {
        return AK_STATUS_INVALID_PARAMETER;
    }

    if (params->TargetId > YUMEDISK_MAX_USABLE_TARGET_ID) {
        return AK_STATUS_INVALID_PARAMETER;
    }

    if ((media_ops->read_bytes == NULL) || (media_ops->stage_write == NULL)) {
        return AK_STATUS_INVALID_PARAMETER;
    }

    return AK_STATUS_SUCCESS;
}

static void AkDiskInitializeState(
    AK_DISK* disk)
{
    disk->State.Lifecycle = AkStateInit;
    disk->State.TargetId = disk->Params.TargetId;
    disk->State.DiskRuntimeId = 0ull;
    disk->State.ReadWorkersRunning = FALSE;
    disk->State.WriteWorkersRunning = FALSE;
    disk->State.AckFlusherRunning = FALSE;
    disk->State.LastError = AK_STATUS_SUCCESS;
}

static void AkDiskSetLastError(
    AK_DISK* disk,
    AK_STATUS status)
{
    AcquireSRWLockExclusive(&disk->Lock);
    disk->State.LastError = status;
    ReleaseSRWLockExclusive(&disk->Lock);
}

static AK_STATUS AkDiskAllocateWorkerArrays(
    AK_DISK* disk)
{
    size_t read_count;
    size_t write_count;

    read_count = (size_t)disk->Params.ReadWorkerCount;
    write_count = (size_t)disk->Params.WriteWorkerCount;

    disk->ReadWorkerThreads = (HANDLE*)AkAllocZero(read_count * sizeof(HANDLE));
    disk->ReadWorkerContexts = (AK_DISK_WORKER_CONTEXT*)AkAllocZero(
        read_count * sizeof(AK_DISK_WORKER_CONTEXT));
    disk->WriteWorkerThreads = (HANDLE*)AkAllocZero(write_count * sizeof(HANDLE));
    disk->WriteWorkerContexts = (AK_DISK_WORKER_CONTEXT*)AkAllocZero(
        write_count * sizeof(AK_DISK_WORKER_CONTEXT));

    if ((disk->ReadWorkerThreads == NULL) || (disk->ReadWorkerContexts == NULL) ||
        (disk->WriteWorkerThreads == NULL) || (disk->WriteWorkerContexts == NULL)) {
        AkDiskFreeWorkerArrays(disk);
        return AK_STATUS_INSUFFICIENT_RESOURCES;
    }

    return AK_STATUS_SUCCESS;
}

static void AkDiskFreeWorkerArrays(
    AK_DISK* disk)
{
    AkFree(disk->ReadWorkerThreads);
    AkFree(disk->ReadWorkerContexts);
    AkFree(disk->WriteWorkerThreads);
    AkFree(disk->WriteWorkerContexts);

    disk->ReadWorkerThreads = NULL;
    disk->ReadWorkerContexts = NULL;
    disk->WriteWorkerThreads = NULL;
    disk->WriteWorkerContexts = NULL;
}

static AK_STATUS AkDiskStartWorkerArray(
    AK_DISK* disk,
    HANDLE* threads,
    AK_DISK_WORKER_CONTEXT* contexts,
    UINT32 worker_count,
    UINT32 worker_kind)
{
    UINT32 index;

    for (index = 0u; index < worker_count; ++index) {
        contexts[index].Disk = disk;
        contexts[index].WorkerIndex = index;
        contexts[index].WorkerKind = worker_kind;
        threads[index] = CreateThread(
            NULL,
            0u,
            AkDiskWorkerThreadProc,
            &contexts[index],
            0u,
            NULL);
        if (threads[index] == NULL) {
            return AkFromWin32Error(GetLastError());
        }
    }

    return AK_STATUS_SUCCESS;
}

static void AkDiskJoinWorkerArray(
    HANDLE* threads,
    UINT32 worker_count)
{
    UINT32 index;

    if (threads == NULL) {
        return;
    }

    for (index = 0u; index < worker_count; ++index) {
        if (threads[index] != NULL) {
            (void)WaitForSingleObject(threads[index], INFINITE);
            CloseHandle(threads[index]);
            threads[index] = NULL;
        }
    }
}

static AK_STATUS AkDiskStartWorkers(
    AK_DISK* disk)
{
    AK_STATUS status;

    status = AkDiskAllocateWorkerArrays(disk);
    if (status != AK_STATUS_SUCCESS) {
        return status;
    }

    status = AkDiskStartWorkerArray(
        disk,
        disk->ReadWorkerThreads,
        disk->ReadWorkerContexts,
        disk->Params.ReadWorkerCount,
        AK_DISK_WORKER_KIND_READ);
    if (status != AK_STATUS_SUCCESS) {
        AkDiskStopWorkers(disk);
        return status;
    }

    status = AkDiskStartWorkerArray(
        disk,
        disk->WriteWorkerThreads,
        disk->WriteWorkerContexts,
        disk->Params.WriteWorkerCount,
        AK_DISK_WORKER_KIND_WRITE);
    if (status != AK_STATUS_SUCCESS) {
        AkDiskStopWorkers(disk);
        return status;
    }

    disk->AckFlusherContext.Disk = disk;
    disk->AckFlusherContext.WorkerIndex = 0u;
    disk->AckFlusherContext.WorkerKind = AK_DISK_WORKER_KIND_ACK;
    disk->AckFlusherThread = CreateThread(
        NULL,
        0u,
        AkDiskWorkerThreadProc,
        &disk->AckFlusherContext,
        0u,
        NULL);
    if (disk->AckFlusherThread == NULL) {
        status = AkFromWin32Error(GetLastError());
        AkDiskStopWorkers(disk);
        return status;
    }

    AcquireSRWLockExclusive(&disk->Lock);
    disk->State.ReadWorkersRunning = TRUE;
    disk->State.WriteWorkersRunning = TRUE;
    disk->State.AckFlusherRunning = TRUE;
    ReleaseSRWLockExclusive(&disk->Lock);

    return AK_STATUS_SUCCESS;
}

static void AkDiskStopWorkers(
    AK_DISK* disk)
{
    if (disk->StopEvent != NULL) {
        SetEvent(disk->StopEvent);
    }

    AkDiskJoinWorkerArray(disk->ReadWorkerThreads, disk->Params.ReadWorkerCount);
    AkDiskJoinWorkerArray(disk->WriteWorkerThreads, disk->Params.WriteWorkerCount);

    if (disk->AckFlusherThread != NULL) {
        (void)WaitForSingleObject(disk->AckFlusherThread, INFINITE);
        CloseHandle(disk->AckFlusherThread);
        disk->AckFlusherThread = NULL;
    }

    AkDiskFreeWorkerArrays(disk);

    AcquireSRWLockExclusive(&disk->Lock);
    disk->State.ReadWorkersRunning = FALSE;
    disk->State.WriteWorkersRunning = FALSE;
    disk->State.AckFlusherRunning = FALSE;
    ReleaseSRWLockExclusive(&disk->Lock);
}

static AK_STATUS AkDiskPrimeReadAvailability(
    AK_DISK* disk)
{
    AcquireSRWLockExclusive(&disk->Lock);
    disk->PrimedReadSlotDepth = disk->Params.QueueDepth;
    disk->Stats.ReadSlotPosts = disk->Params.QueueDepth;
    ReleaseSRWLockExclusive(&disk->Lock);
    return AK_STATUS_SUCCESS;
}

static void AkDiskDestroy(
    AK_DISK* disk)
{
    if (disk == NULL) {
        return;
    }

    if (disk->RegisteredInSession && (disk->Session != NULL)) {
        AkSessionUnregisterDisk(disk->Session, disk);
    }

    AkDiskStopWorkers(disk);

    if (disk->StopEvent != NULL) {
        CloseHandle(disk->StopEvent);
        disk->StopEvent = NULL;
    }
    AkFree(disk);
}

void AkDiskDestroyDetached(
    AK_DISK* disk)
{
    AkDiskDestroy(disk);
}

static AK_STATUS AkDiskEmitLifecycleEvent(
    AK_DISK* disk,
    AK_EVENT_TYPE type,
    AK_STATUS status)
{
    AK_EVENT event_record;

    (void)memset(&event_record, 0, sizeof(event_record));
    event_record.Type = type;
    event_record.TargetId = disk->State.TargetId;
    event_record.DiskRuntimeId = disk->State.DiskRuntimeId;
    event_record.Status = status;
    return AkEventQueuePush(disk->Session, &event_record);
}

AK_STATUS AkDiskCreate(
    AK_SESSION* session,
    const AK_DISK_PARAMS* params,
    const AK_MEDIA_OPS* media_ops,
    void* media_ctx,
    AK_DISK** out_disk)
{
    AK_DISK* disk;
    HANDLE control_file;
    UINT64 session_id;
    UINT64 runtime_id;
    AK_STATUS status;

    if (out_disk == NULL) {
        return AK_STATUS_INVALID_PARAMETER;
    }

    *out_disk = NULL;

    status = AkDiskValidateParams(params, media_ops);
    if (status != AK_STATUS_SUCCESS) {
        return status;
    }

    if (session == NULL) {
        return AK_STATUS_INVALID_PARAMETER;
    }

    disk = (AK_DISK*)AkAllocZero(sizeof(*disk));
    if (disk == NULL) {
        return AK_STATUS_INSUFFICIENT_RESOURCES;
    }

    disk->Session = session;
    disk->Params = *params;
    disk->MediaOps = *media_ops;
    disk->MediaCtx = media_ctx;
    InitializeSRWLock(&disk->Lock);
    AkDiskInitializeState(disk);

    disk->StopEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (disk->StopEvent == NULL) {
        status = AkFromWin32Error(GetLastError());
        AkDiskDestroy(disk);
        return status;
    }

    AcquireSRWLockExclusive(&disk->Lock);
    disk->State.Lifecycle = AkStateStarting;
    ReleaseSRWLockExclusive(&disk->Lock);

    status = AkSessionAcquireTransport(session, &control_file, &session_id);
    if (status != AK_STATUS_SUCCESS) {
        AkDiskSetLastError(disk, status);
        AkDiskDestroy(disk);
        return status;
    }

    status = AkSessionRegisterDisk(session, disk, &runtime_id);
    if (status != AK_STATUS_SUCCESS) {
        AkDiskSetLastError(disk, status);
        AkDiskDestroy(disk);
        return status;
    }

    (void)runtime_id;

    status = AkDiskStartWorkers(disk);
    if (status != AK_STATUS_SUCCESS) {
        AkDiskSetLastError(disk, status);
        AkSessionUnregisterDisk(session, disk);
        AkDiskDestroy(disk);
        return status;
    }

    status = AkDiskPrimeReadAvailability(disk);
    if (status != AK_STATUS_SUCCESS) {
        AkDiskSetLastError(disk, status);
        AkDiskStopWorkers(disk);
        AkSessionUnregisterDisk(session, disk);
        AkDiskDestroy(disk);
        return status;
    }

    status = AkProtocolCreateDisk(control_file, session_id, &disk->Params);
    if (status != AK_STATUS_SUCCESS) {
        AkDiskSetLastError(disk, status);
        AkDiskStopWorkers(disk);
        AkSessionUnregisterDisk(session, disk);
        AkDiskDestroy(disk);
        return status;
    }

    AcquireSRWLockExclusive(&disk->Lock);
    disk->State.Lifecycle = AkStateRunning;
    disk->State.LastError = AK_STATUS_SUCCESS;
    ReleaseSRWLockExclusive(&disk->Lock);

    status = AkDiskEmitLifecycleEvent(disk, AkEventDiskOnline, AK_STATUS_SUCCESS);
    if (status != AK_STATUS_SUCCESS) {
        AkDiskSetLastError(disk, status);
        AcquireSRWLockExclusive(&disk->Lock);
        disk->State.Lifecycle = AkStateBroken;
        ReleaseSRWLockExclusive(&disk->Lock);
        AkDiskStopWorkers(disk);
        (void)AkProtocolRemoveDisk(control_file, session_id, disk->Params.TargetId, 0u);
        AkSessionUnregisterDisk(session, disk);
        AkDiskDestroy(disk);
        return status;
    }

    *out_disk = disk;
    return AK_STATUS_SUCCESS;
}

AK_STATUS AkDiskRemove(AK_DISK* disk)
{
    HANDLE control_file;
    UINT64 session_id;
    AK_STATUS status;
    AK_STATUS remove_status;
    BOOLEAN already_closed;

    if (disk == NULL) {
        return AK_STATUS_INVALID_PARAMETER;
    }

    AcquireSRWLockExclusive(&disk->Lock);
    already_closed = (BOOLEAN)((disk->State.Lifecycle == AkStateClosed) ||
                               (disk->State.Lifecycle == AkStateRemoving));
    if (!already_closed) {
        disk->State.Lifecycle = AkStateRemoving;
        disk->State.LastError = AK_STATUS_SUCCESS;
    }
    ReleaseSRWLockExclusive(&disk->Lock);

    if (already_closed) {
        return AK_STATUS_SUCCESS;
    }

    status = AkSessionAcquireTransport(disk->Session, &control_file, &session_id);
    if (status != AK_STATUS_SUCCESS) {
        AkDiskSetLastError(disk, status);
        control_file = NULL;
        session_id = 0ull;
    }

    AkDiskStopWorkers(disk);

    remove_status = AK_STATUS_SUCCESS;
    if ((control_file != NULL) && (control_file != INVALID_HANDLE_VALUE) && (session_id != 0ull)) {
        remove_status = AkProtocolRemoveDisk(control_file, session_id, disk->Params.TargetId, 0u);
    }

    if (remove_status != AK_STATUS_SUCCESS) {
        AkDiskSetLastError(disk, remove_status);
    }

    status = AkDiskEmitLifecycleEvent(disk, AkEventDiskRemoved, remove_status);
    if (status != AK_STATUS_SUCCESS) {
        AkDiskSetLastError(disk, status);
    }

    AcquireSRWLockExclusive(&disk->Lock);
    disk->State.Lifecycle = AkStateClosed;
    ReleaseSRWLockExclusive(&disk->Lock);

    AkSessionUnregisterDisk(disk->Session, disk);
    AkDiskDestroy(disk);
    return remove_status;
}

AK_STATUS AkDiskQueryState(
    AK_DISK* disk,
    AK_DISK_STATE* out_state)
{
    if ((disk == NULL) || (out_state == NULL)) {
        return AK_STATUS_INVALID_PARAMETER;
    }

    AcquireSRWLockShared(&disk->Lock);
    *out_state = disk->State;
    ReleaseSRWLockShared(&disk->Lock);
    return AK_STATUS_SUCCESS;
}

AK_STATUS AkDiskQueryStats(
    AK_DISK* disk,
    AK_DISK_STATS* out_stats)
{
    if ((disk == NULL) || (out_stats == NULL)) {
        return AK_STATUS_INVALID_PARAMETER;
    }

    AcquireSRWLockShared(&disk->Lock);
    *out_stats = disk->Stats;
    ReleaseSRWLockShared(&disk->Lock);
    return AK_STATUS_SUCCESS;
}
