#include "event/ak_event.h"

#include <string.h>

static AK_STATUS AkEventQueueGrowLocked(
    AK_SESSION* session)
{
    AK_EVENT* old_items;
    AK_EVENT* new_items;
    UINT32 old_capacity;
    UINT32 new_capacity;
    UINT32 index;

    old_capacity = session->EventQueue.Capacity;
    if (old_capacity == 0u) {
        new_capacity = session->EventQueue.InitialCapacity;
        if (new_capacity == 0u) {
            new_capacity = 16u;
        }
    } else {
        if (old_capacity > (UINT32)(0xFFFFFFFFu / 2u)) {
            return AK_STATUS_INSUFFICIENT_RESOURCES;
        }

        new_capacity = old_capacity * 2u;
    }

    new_items = (AK_EVENT*)AkAllocZero((size_t)new_capacity * sizeof(AK_EVENT));
    if (new_items == NULL) {
        return AK_STATUS_INSUFFICIENT_RESOURCES;
    }

    old_items = session->EventQueue.Items;
    for (index = 0u; index < session->EventQueue.Count; ++index) {
        UINT32 old_index;

        old_index = (session->EventQueue.Head + index) % old_capacity;
        new_items[index] = old_items[old_index];
    }

    session->EventQueue.Items = new_items;
    session->EventQueue.Capacity = new_capacity;
    session->EventQueue.Head = 0u;

    AkFree(old_items);
    return AK_STATUS_SUCCESS;
}

static AK_STATUS AkEventQueuePopLocked(
    AK_SESSION* session,
    AK_EVENT* out_event)
{
    UINT32 index;

    if (session->EventQueue.Count == 0u) {
        return AK_STATUS_NO_MORE_ENTRIES;
    }

    index = session->EventQueue.Head;
    *out_event = session->EventQueue.Items[index];
    session->EventQueue.Head = (index + 1u) % session->EventQueue.Capacity;
    session->EventQueue.Count -= 1u;

    if (session->EventQueue.Count == 0u) {
        ResetEvent(session->EventQueue.WaitEvent);
    }

    return AK_STATUS_SUCCESS;
}

AK_STATUS AkEventQueueInitialize(
    AK_SESSION* session)
{
    UINT32 initial_capacity;

    if (session == NULL) {
        return AK_STATUS_INVALID_PARAMETER;
    }

    initial_capacity = session->EventQueue.InitialCapacity;
    if (initial_capacity == 0u) {
        return AK_STATUS_INVALID_PARAMETER;
    }

    session->EventQueue.Items = (AK_EVENT*)AkAllocZero((size_t)initial_capacity * sizeof(AK_EVENT));
    if (session->EventQueue.Items == NULL) {
        return AK_STATUS_INSUFFICIENT_RESOURCES;
    }

    session->EventQueue.WaitEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (session->EventQueue.WaitEvent == NULL) {
        AkFree(session->EventQueue.Items);
        session->EventQueue.Items = NULL;
        return AkFromWin32Error(GetLastError());
    }

    session->EventQueue.Capacity = initial_capacity;
    session->EventQueue.Head = 0u;
    session->EventQueue.Count = 0u;
    session->EventQueue.SessionBrokenQueued = FALSE;
    return AK_STATUS_SUCCESS;
}

void AkEventQueueDestroy(
    AK_SESSION* session)
{
    if (session == NULL) {
        return;
    }

    if (session->EventQueue.WaitEvent != NULL) {
        CloseHandle(session->EventQueue.WaitEvent);
        session->EventQueue.WaitEvent = NULL;
    }

    AkFree(session->EventQueue.Items);
    session->EventQueue.Items = NULL;
    session->EventQueue.Capacity = 0u;
    session->EventQueue.Head = 0u;
    session->EventQueue.Count = 0u;
    session->EventQueue.SessionBrokenQueued = FALSE;
}

AK_STATUS AkEventQueuePush(
    AK_SESSION* session,
    const AK_EVENT* event_record)
{
    AK_STATUS status;
    UINT32 tail;

    if ((session == NULL) || (event_record == NULL)) {
        return AK_STATUS_INVALID_PARAMETER;
    }

    AcquireSRWLockExclusive(&session->Lock);

    if (session->EventQueue.Count == session->EventQueue.Capacity) {
        status = AkEventQueueGrowLocked(session);
        if (status != AK_STATUS_SUCCESS) {
            session->Stats.EventsDropped += 1ull;
            session->Stats.ProtocolFailures += 1ull;
            session->State.Lifecycle = AkStateBroken;
            session->State.LastError = status;
            session->State.TransportReady = FALSE;
            session->State.HeartbeatRunning = FALSE;
            ReleaseSRWLockExclusive(&session->Lock);
            return status;
        }
    }

    tail = (session->EventQueue.Head + session->EventQueue.Count) % session->EventQueue.Capacity;
    session->EventQueue.Items[tail] = *event_record;
    session->EventQueue.Count += 1u;
    session->Stats.EventsQueued += 1ull;
    SetEvent(session->EventQueue.WaitEvent);

    ReleaseSRWLockExclusive(&session->Lock);
    return AK_STATUS_SUCCESS;
}

AK_STATUS AkEventQueuePushSessionBroken(
    AK_SESSION* session,
    AK_STATUS status)
{
    AK_EVENT event_record;
    BOOLEAN should_queue;

    if (session == NULL) {
        return AK_STATUS_INVALID_PARAMETER;
    }

    AcquireSRWLockExclusive(&session->Lock);
    should_queue = (BOOLEAN)(session->EventQueue.SessionBrokenQueued == FALSE);
    if (should_queue) {
        session->EventQueue.SessionBrokenQueued = TRUE;
    }
    ReleaseSRWLockExclusive(&session->Lock);

    if (!should_queue) {
        return AK_STATUS_SUCCESS;
    }

    (void)memset(&event_record, 0, sizeof(event_record));
    event_record.Type = AkEventSessionBroken;
    event_record.Status = status;
    return AkEventQueuePush(session, &event_record);
}

AK_STATUS AkEventWait(
    AK_SESSION* session,
    DWORD timeout_ms,
    AK_EVENT* out_event)
{
    HANDLE wait_handle;
    DWORD wait_status;
    AK_STATUS status;

    if ((session == NULL) || (out_event == NULL)) {
        return AK_STATUS_INVALID_PARAMETER;
    }

    AcquireSRWLockShared(&session->Lock);
    wait_handle = session->EventQueue.WaitEvent;
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
    status = AkEventQueuePopLocked(session, out_event);
    ReleaseSRWLockExclusive(&session->Lock);
    return status;
}

AK_STATUS AkEventPoll(
    AK_SESSION* session,
    AK_EVENT* out_event)
{
    AK_STATUS status;

    if ((session == NULL) || (out_event == NULL)) {
        return AK_STATUS_INVALID_PARAMETER;
    }

    AcquireSRWLockExclusive(&session->Lock);
    status = AkEventQueuePopLocked(session, out_event);
    ReleaseSRWLockExclusive(&session->Lock);
    return status;
}
