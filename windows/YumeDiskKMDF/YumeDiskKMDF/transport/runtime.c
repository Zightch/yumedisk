#include "runtime.h"

#include "..\core\memory.h"

typedef enum _CTRL_TRANSPORT_RUNTIME_STATE {
    CtrlTransportRuntimeStopped = 0,
    CtrlTransportRuntimeRunning = 1,
    CtrlTransportRuntimeClosing = 2
} CTRL_TRANSPORT_RUNTIME_STATE;

struct _CTRL_TRANSPORT_RUNTIME {
    PCTRL_FILE_CONTEXT SessionContext;
    WDFSPINLOCK FreeListLock;
    WDFSPINLOCK SubmitQueueLock;
    LIST_ENTRY FreeList;
    LIST_ENTRY SubmitQueue;
    KEVENT FreeEvent;
    KEVENT SubmitEvent;
    KEVENT StopEvent;
    KEVENT ActiveZeroEvent;
    volatile LONG State;
    volatile LONG ActiveCount;
    volatile LONG PoolObjectCount;
    volatile LONG PoolHighWater;
    volatile LONG PoolAcquireFailures;
    volatile LONG SubmitQueued;
    volatile LONG SubmitCompleted;
};

static
VOID
ControlTransportRuntimeDeleteLocks(
    _Inout_ PCTRL_TRANSPORT_RUNTIME Runtime
)
{
    if (Runtime->SubmitQueueLock != NULL) {
        WdfObjectDelete(Runtime->SubmitQueueLock);
        Runtime->SubmitQueueLock = NULL;
    }

    if (Runtime->FreeListLock != NULL) {
        WdfObjectDelete(Runtime->FreeListLock);
        Runtime->FreeListLock = NULL;
    }
}

static
VOID
ControlTransportRuntimeDrainActive(
    _Inout_ PCTRL_TRANSPORT_RUNTIME Runtime
)
{
    while (InterlockedCompareExchange(&Runtime->ActiveCount, 0, 0) != 0) {
        KeWaitForSingleObject(&Runtime->ActiveZeroEvent, Executive, KernelMode, FALSE, NULL);
    }
}

NTSTATUS
ControlTransportRuntimeStart(
    _Inout_ PCTRL_FILE_CONTEXT Context,
    _In_ WDFFILEOBJECT FileObject
)
{
    WDF_OBJECT_ATTRIBUTES attributes;
    PCTRL_TRANSPORT_RUNTIME runtime;
    NTSTATUS status;

    if (Context == NULL || FileObject == NULL) {
        return STATUS_INVALID_PARAMETER;
    }

    if (Context->TransportRuntime != NULL) {
        return STATUS_DEVICE_BUSY;
    }

    runtime = (PCTRL_TRANSPORT_RUNTIME)ControlAlloc(sizeof(*runtime));
    if (runtime == NULL) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    RtlZeroMemory(runtime, sizeof(*runtime));
    runtime->SessionContext = Context;
    runtime->State = CtrlTransportRuntimeRunning;
    runtime->ActiveCount = 0;
    InitializeListHead(&runtime->FreeList);
    InitializeListHead(&runtime->SubmitQueue);
    KeInitializeEvent(&runtime->FreeEvent, NotificationEvent, FALSE);
    KeInitializeEvent(&runtime->SubmitEvent, NotificationEvent, FALSE);
    KeInitializeEvent(&runtime->StopEvent, NotificationEvent, FALSE);
    KeInitializeEvent(&runtime->ActiveZeroEvent, NotificationEvent, TRUE);

    WDF_OBJECT_ATTRIBUTES_INIT(&attributes);
    attributes.ParentObject = FileObject;
    status = WdfSpinLockCreate(&attributes, &runtime->FreeListLock);
    if (!NT_SUCCESS(status)) {
        ControlFree(runtime);
        return status;
    }

    WDF_OBJECT_ATTRIBUTES_INIT(&attributes);
    attributes.ParentObject = FileObject;
    status = WdfSpinLockCreate(&attributes, &runtime->SubmitQueueLock);
    if (!NT_SUCCESS(status)) {
        ControlTransportRuntimeDeleteLocks(runtime);
        ControlFree(runtime);
        return status;
    }

    if (InterlockedCompareExchangePointer((PVOID volatile*)&Context->TransportRuntime, runtime, NULL) != NULL) {
        ControlTransportRuntimeDeleteLocks(runtime);
        ControlFree(runtime);
        return STATUS_DEVICE_BUSY;
    }

    return STATUS_SUCCESS;
}

VOID
ControlTransportRuntimeBeginClose(
    _Inout_ PCTRL_FILE_CONTEXT Context
)
{
    PCTRL_TRANSPORT_RUNTIME runtime;

    if (Context == NULL) {
        return;
    }

    runtime = Context->TransportRuntime;
    if (runtime == NULL) {
        return;
    }

    InterlockedExchange(&runtime->State, CtrlTransportRuntimeClosing);
    KeSetEvent(&runtime->StopEvent, IO_NO_INCREMENT, FALSE);
}

VOID
ControlTransportRuntimeStop(
    _Inout_ PCTRL_FILE_CONTEXT Context
)
{
    PCTRL_TRANSPORT_RUNTIME runtime;

    if (Context == NULL) {
        return;
    }

    runtime = (PCTRL_TRANSPORT_RUNTIME)InterlockedExchangePointer(
        (PVOID volatile*)&Context->TransportRuntime,
        NULL);
    if (runtime == NULL) {
        return;
    }

    InterlockedExchange(&runtime->State, CtrlTransportRuntimeClosing);
    KeSetEvent(&runtime->StopEvent, IO_NO_INCREMENT, FALSE);
    ControlTransportRuntimeDrainActive(runtime);
    InterlockedExchange(&runtime->State, CtrlTransportRuntimeStopped);
    ControlTransportRuntimeDeleteLocks(runtime);
    ControlFree(runtime);
}
