#include "event/ak_event.h"

#include <string.h>

static AK_STATUS AkResponseQueueGrowLocked(
    AK_SESSION* session)
{
    AK_RESPONSE* old_items;
    AK_RESPONSE* new_items;
    UINT32 old_capacity;
    UINT32 new_capacity;
    UINT32 index;

    old_capacity = session->ResponseQueue.Capacity;
    if (old_capacity == 0u) {
        new_capacity = session->ResponseQueue.InitialCapacity;
        if (new_capacity == 0u) {
            new_capacity = 16u;
        }
    } else {
        if (old_capacity > (UINT32)(0xFFFFFFFFu / 2u)) {
            return AK_STATUS_INSUFFICIENT_RESOURCES;
        }

        new_capacity = old_capacity * 2u;
    }

    new_items = (AK_RESPONSE*)AkAllocZero((size_t)new_capacity * sizeof(AK_RESPONSE));
    if (new_items == NULL) {
        return AK_STATUS_INSUFFICIENT_RESOURCES;
    }

    old_items = session->ResponseQueue.Items;
    for (index = 0u; index < session->ResponseQueue.Count; ++index) {
        UINT32 old_index;

        old_index = (session->ResponseQueue.Head + index) % old_capacity;
        new_items[index] = old_items[old_index];
    }

    session->ResponseQueue.Items = new_items;
    session->ResponseQueue.Capacity = new_capacity;
    session->ResponseQueue.Head = 0u;

    AkFree(old_items);
    return AK_STATUS_SUCCESS;
}

static AK_STATUS AkResponseQueuePopLocked(
    AK_SESSION* session,
    AK_RESPONSE* out_response)
{
    UINT32 index;

    if (session->ResponseQueue.Count == 0u) {
        return AK_STATUS_NO_MORE_ENTRIES;
    }

    index = session->ResponseQueue.Head;
    *out_response = session->ResponseQueue.Items[index];
    session->ResponseQueue.Head = (index + 1u) % session->ResponseQueue.Capacity;
    session->ResponseQueue.Count -= 1u;

    if (session->ResponseQueue.Count == 0u) {
        ResetEvent(session->ResponseQueue.WaitEvent);
    }

    return AK_STATUS_SUCCESS;
}

static AK_STATUS AkSessionNoticeQueueGrowLocked(
    AK_SESSION* session)
{
    AK_SESSION_NOTICE* old_items;
    AK_SESSION_NOTICE* new_items;
    UINT32 old_capacity;
    UINT32 new_capacity;
    UINT32 index;

    old_capacity = session->SessionNoticeQueue.Capacity;
    if (old_capacity == 0u) {
        new_capacity = session->SessionNoticeQueue.InitialCapacity;
        if (new_capacity == 0u) {
            new_capacity = 4u;
        }
    } else {
        if (old_capacity > (UINT32)(0xFFFFFFFFu / 2u)) {
            return AK_STATUS_INSUFFICIENT_RESOURCES;
        }

        new_capacity = old_capacity * 2u;
    }

    new_items = (AK_SESSION_NOTICE*)AkAllocZero(
        (size_t)new_capacity * sizeof(AK_SESSION_NOTICE));
    if (new_items == NULL) {
        return AK_STATUS_INSUFFICIENT_RESOURCES;
    }

    old_items = session->SessionNoticeQueue.Items;
    for (index = 0u; index < session->SessionNoticeQueue.Count; ++index) {
        UINT32 old_index;

        old_index = (session->SessionNoticeQueue.Head + index) % old_capacity;
        new_items[index] = old_items[old_index];
    }

    session->SessionNoticeQueue.Items = new_items;
    session->SessionNoticeQueue.Capacity = new_capacity;
    session->SessionNoticeQueue.Head = 0u;

    AkFree(old_items);
    return AK_STATUS_SUCCESS;
}

static AK_STATUS AkSessionNoticeQueuePopLocked(
    AK_SESSION* session,
    AK_SESSION_NOTICE* out_notice)
{
    UINT32 index;

    if (session->SessionNoticeQueue.Count == 0u) {
        return AK_STATUS_NO_MORE_ENTRIES;
    }

    index = session->SessionNoticeQueue.Head;
    *out_notice = session->SessionNoticeQueue.Items[index];
    session->SessionNoticeQueue.Head =
        (index + 1u) % session->SessionNoticeQueue.Capacity;
    session->SessionNoticeQueue.Count -= 1u;

    if (session->SessionNoticeQueue.Count == 0u) {
        ResetEvent(session->SessionNoticeQueue.WaitEvent);
    }

    return AK_STATUS_SUCCESS;
}

AK_STATUS AkResponseQueueInitialize(
    AK_SESSION* session)
{
    UINT32 initial_capacity;

    if (session == NULL) {
        return AK_STATUS_INVALID_PARAMETER;
    }

    initial_capacity = session->ResponseQueue.InitialCapacity;
    if (initial_capacity == 0u) {
        return AK_STATUS_INVALID_PARAMETER;
    }

    session->ResponseQueue.Items = (AK_RESPONSE*)AkAllocZero(
        (size_t)initial_capacity * sizeof(AK_RESPONSE));
    if (session->ResponseQueue.Items == NULL) {
        return AK_STATUS_INSUFFICIENT_RESOURCES;
    }

    session->ResponseQueue.WaitEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (session->ResponseQueue.WaitEvent == NULL) {
        AkFree(session->ResponseQueue.Items);
        session->ResponseQueue.Items = NULL;
        return AkFromWin32Error(GetLastError());
    }

    session->ResponseQueue.Capacity = initial_capacity;
    session->ResponseQueue.Head = 0u;
    session->ResponseQueue.Count = 0u;
    return AK_STATUS_SUCCESS;
}

void AkResponseQueueDestroy(
    AK_SESSION* session)
{
    if (session == NULL) {
        return;
    }

    if (session->ResponseQueue.WaitEvent != NULL) {
        CloseHandle(session->ResponseQueue.WaitEvent);
        session->ResponseQueue.WaitEvent = NULL;
    }

    AkFree(session->ResponseQueue.Items);
    session->ResponseQueue.Items = NULL;
    session->ResponseQueue.Capacity = 0u;
    session->ResponseQueue.Head = 0u;
    session->ResponseQueue.Count = 0u;
}

AK_STATUS AkResponseQueuePush(
    AK_SESSION* session,
    const AK_RESPONSE* response_record)
{
    AK_STATUS status;
    UINT32 tail;

    if ((session == NULL) || (response_record == NULL)) {
        return AK_STATUS_INVALID_PARAMETER;
    }

    AcquireSRWLockExclusive(&session->Lock);

    if (session->ResponseQueue.Count == session->ResponseQueue.Capacity) {
        status = AkResponseQueueGrowLocked(session);
        if (status != AK_STATUS_SUCCESS) {
            session->Stats.ResponsesDropped += 1ull;
            session->Stats.ProtocolFailures += 1ull;
            session->State.Lifecycle = AkStateBroken;
            session->State.LastError = status;
            session->State.TransportReady = FALSE;
            session->State.HeartbeatRunning = FALSE;
            ReleaseSRWLockExclusive(&session->Lock);
            (void)AkSessionNoticeQueuePushBroken(session, status);
            return status;
        }
    }

    tail = (session->ResponseQueue.Head + session->ResponseQueue.Count) %
        session->ResponseQueue.Capacity;
    session->ResponseQueue.Items[tail] = *response_record;
    session->ResponseQueue.Count += 1u;
    session->Stats.ResponsesQueued += 1ull;
    SetEvent(session->ResponseQueue.WaitEvent);

    ReleaseSRWLockExclusive(&session->Lock);
    return AK_STATUS_SUCCESS;
}

AK_STATUS AkResponseWait(
    AK_SESSION* session,
    DWORD timeout_ms,
    AK_RESPONSE* out_response)
{
    HANDLE wait_handle;
    DWORD wait_status;
    AK_STATUS status;

    if ((session == NULL) || (out_response == NULL)) {
        return AK_STATUS_INVALID_PARAMETER;
    }

    AcquireSRWLockShared(&session->Lock);
    wait_handle = session->ResponseQueue.WaitEvent;
    ReleaseSRWLockShared(&session->Lock);

    if (wait_handle == NULL) {
        return AK_STATUS_DEVICE_NOT_READY;
    }

    wait_status = WaitForSingleObject(wait_handle, timeout_ms);
    if (wait_status == WAIT_TIMEOUT) {
        return AK_STATUS_TIMEOUT;
    }

    if (wait_status != WAIT_OBJECT_0) {
        return AkFromWin32Error(GetLastError());
    }

    AcquireSRWLockExclusive(&session->Lock);
    status = AkResponseQueuePopLocked(session, out_response);
    ReleaseSRWLockExclusive(&session->Lock);
    return status;
}

AK_STATUS AkResponsePoll(
    AK_SESSION* session,
    AK_RESPONSE* out_response)
{
    AK_STATUS status;

    if ((session == NULL) || (out_response == NULL)) {
        return AK_STATUS_INVALID_PARAMETER;
    }

    AcquireSRWLockExclusive(&session->Lock);
    status = AkResponseQueuePopLocked(session, out_response);
    ReleaseSRWLockExclusive(&session->Lock);
    return status;
}

AK_STATUS AkSessionNoticeQueueInitialize(
    AK_SESSION* session)
{
    UINT32 initial_capacity;

    if (session == NULL) {
        return AK_STATUS_INVALID_PARAMETER;
    }

    initial_capacity = session->SessionNoticeQueue.InitialCapacity;
    if (initial_capacity == 0u) {
        return AK_STATUS_INVALID_PARAMETER;
    }

    session->SessionNoticeQueue.Items = (AK_SESSION_NOTICE*)AkAllocZero(
        (size_t)initial_capacity * sizeof(AK_SESSION_NOTICE));
    if (session->SessionNoticeQueue.Items == NULL) {
        return AK_STATUS_INSUFFICIENT_RESOURCES;
    }

    session->SessionNoticeQueue.WaitEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (session->SessionNoticeQueue.WaitEvent == NULL) {
        AkFree(session->SessionNoticeQueue.Items);
        session->SessionNoticeQueue.Items = NULL;
        return AkFromWin32Error(GetLastError());
    }

    session->SessionNoticeQueue.Capacity = initial_capacity;
    session->SessionNoticeQueue.Head = 0u;
    session->SessionNoticeQueue.Count = 0u;
    session->SessionNoticeQueue.BrokenQueued = FALSE;
    return AK_STATUS_SUCCESS;
}

void AkSessionNoticeQueueDestroy(
    AK_SESSION* session)
{
    if (session == NULL) {
        return;
    }

    if (session->SessionNoticeQueue.WaitEvent != NULL) {
        CloseHandle(session->SessionNoticeQueue.WaitEvent);
        session->SessionNoticeQueue.WaitEvent = NULL;
    }

    AkFree(session->SessionNoticeQueue.Items);
    session->SessionNoticeQueue.Items = NULL;
    session->SessionNoticeQueue.Capacity = 0u;
    session->SessionNoticeQueue.Head = 0u;
    session->SessionNoticeQueue.Count = 0u;
    session->SessionNoticeQueue.BrokenQueued = FALSE;
}

AK_STATUS AkSessionNoticeQueuePush(
    AK_SESSION* session,
    const AK_SESSION_NOTICE* notice_record)
{
    AK_STATUS status;
    UINT32 tail;

    if ((session == NULL) || (notice_record == NULL)) {
        return AK_STATUS_INVALID_PARAMETER;
    }

    AcquireSRWLockExclusive(&session->Lock);

    if (session->SessionNoticeQueue.Count == session->SessionNoticeQueue.Capacity) {
        status = AkSessionNoticeQueueGrowLocked(session);
        if (status != AK_STATUS_SUCCESS) {
            session->Stats.SessionNoticesDropped += 1ull;
            session->Stats.ProtocolFailures += 1ull;
            session->State.Lifecycle = AkStateBroken;
            session->State.LastError = status;
            session->State.TransportReady = FALSE;
            session->State.HeartbeatRunning = FALSE;
            ReleaseSRWLockExclusive(&session->Lock);
            return status;
        }
    }

    tail = (session->SessionNoticeQueue.Head + session->SessionNoticeQueue.Count) %
        session->SessionNoticeQueue.Capacity;
    session->SessionNoticeQueue.Items[tail] = *notice_record;
    session->SessionNoticeQueue.Count += 1u;
    session->Stats.SessionNoticesQueued += 1ull;
    SetEvent(session->SessionNoticeQueue.WaitEvent);

    ReleaseSRWLockExclusive(&session->Lock);
    return AK_STATUS_SUCCESS;
}

AK_STATUS AkSessionNoticeQueuePushBroken(
    AK_SESSION* session,
    AK_STATUS status)
{
    AK_SESSION_NOTICE notice_record;
    BOOLEAN should_queue;

    if (session == NULL) {
        return AK_STATUS_INVALID_PARAMETER;
    }

    AcquireSRWLockExclusive(&session->Lock);
    should_queue = (BOOLEAN)(session->SessionNoticeQueue.BrokenQueued == FALSE);
    if (should_queue) {
        session->SessionNoticeQueue.BrokenQueued = TRUE;
    }
    ReleaseSRWLockExclusive(&session->Lock);

    if (!should_queue) {
        return AK_STATUS_SUCCESS;
    }

    (void)memset(&notice_record, 0, sizeof(notice_record));
    notice_record.Type = AkSessionNoticeBroken;
    notice_record.Status = status;
    return AkSessionNoticeQueuePush(session, &notice_record);
}

AK_STATUS AkSessionNoticeWait(
    AK_SESSION* session,
    DWORD timeout_ms,
    AK_SESSION_NOTICE* out_notice)
{
    HANDLE wait_handle;
    DWORD wait_status;
    AK_STATUS status;

    if ((session == NULL) || (out_notice == NULL)) {
        return AK_STATUS_INVALID_PARAMETER;
    }

    AcquireSRWLockShared(&session->Lock);
    wait_handle = session->SessionNoticeQueue.WaitEvent;
    ReleaseSRWLockShared(&session->Lock);

    if (wait_handle == NULL) {
        return AK_STATUS_DEVICE_NOT_READY;
    }

    wait_status = WaitForSingleObject(wait_handle, timeout_ms);
    if (wait_status == WAIT_TIMEOUT) {
        return AK_STATUS_TIMEOUT;
    }

    if (wait_status != WAIT_OBJECT_0) {
        return AkFromWin32Error(GetLastError());
    }

    AcquireSRWLockExclusive(&session->Lock);
    status = AkSessionNoticeQueuePopLocked(session, out_notice);
    ReleaseSRWLockExclusive(&session->Lock);
    return status;
}

AK_STATUS AkSessionNoticePoll(
    AK_SESSION* session,
    AK_SESSION_NOTICE* out_notice)
{
    AK_STATUS status;

    if ((session == NULL) || (out_notice == NULL)) {
        return AK_STATUS_INVALID_PARAMETER;
    }

    AcquireSRWLockExclusive(&session->Lock);
    status = AkSessionNoticeQueuePopLocked(session, out_notice);
    ReleaseSRWLockExclusive(&session->Lock);
    return status;
}
