#include "runtime.h"

#include "..\core\memory.h"

#define CTRL_TRANSPORT_SLOT_POOL_HARD_LIMIT 1024

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
ControlTransportRuntimeReferenceActive(
    _Inout_ PCTRL_TRANSPORT_RUNTIME Runtime
)
{
    if (InterlockedIncrement(&Runtime->ActiveCount) == 1) {
        KeClearEvent(&Runtime->ActiveZeroEvent);
    }
}

static
VOID
ControlTransportRuntimeDereferenceActive(
    _Inout_ PCTRL_TRANSPORT_RUNTIME Runtime
)
{
    if (InterlockedDecrement(&Runtime->ActiveCount) == 0) {
        KeSetEvent(&Runtime->ActiveZeroEvent, IO_NO_INCREMENT, FALSE);
    }
}

static
VOID
ControlTransportRuntimeUpdateHighWater(
    _Inout_ PCTRL_TRANSPORT_RUNTIME Runtime,
    _In_ LONG Value
)
{
    LONG oldValue;

    oldValue = InterlockedCompareExchange(&Runtime->PoolHighWater, 0, 0);
    while (Value > oldValue) {
        LONG previousValue;

        previousValue = InterlockedCompareExchange(&Runtime->PoolHighWater, Value, oldValue);
        if (previousValue == oldValue) {
            break;
        }
        oldValue = previousValue;
    }
}

static
VOID
ControlTransportRuntimeResetSlotRequest(
    _Inout_ PCTRL_ASYNC_SLOT_REQUEST SlotRequest
)
{
    SlotRequest->SessionContext = NULL;
    SlotRequest->Request = NULL;
    SlotRequest->WorkItem = NULL;
    SlotRequest->SlotId = 0;
    SlotRequest->TargetId = 0;
    SlotRequest->SlotType = YumeDiskSlotTypeInvalid;
    SlotRequest->DirectBuffer = NULL;
    SlotRequest->DirectBufferSize = 0;
    RtlZeroMemory(&SlotRequest->IoStatusBlock, sizeof(SlotRequest->IoStatusBlock));
    SlotRequest->CompletionState = 0;
    SlotRequest->CompletionStatus = STATUS_UNSUCCESSFUL;
    SlotRequest->CompletionInformation = 0;

    if (SlotRequest->IoctlBuffer != NULL && SlotRequest->IoctlBufferSize != 0) {
        RtlZeroMemory(SlotRequest->IoctlBuffer, SlotRequest->IoctlBufferSize);
    }
}

static
VOID
ControlTransportRuntimeDestroySlotRequest(
    _Inout_opt_ PCTRL_ASYNC_SLOT_REQUEST SlotRequest
)
{
    if (SlotRequest == NULL) {
        return;
    }

    if (SlotRequest->WorkItem != NULL) {
        IoFreeWorkItem(SlotRequest->WorkItem);
        SlotRequest->WorkItem = NULL;
    }
    if (SlotRequest->Irp != NULL) {
        IoFreeIrp(SlotRequest->Irp);
        SlotRequest->Irp = NULL;
    }
    ControlFree(SlotRequest->IoctlBuffer);
    SlotRequest->IoctlBuffer = NULL;
    ControlFree(SlotRequest);
}

static
NTSTATUS
ControlTransportRuntimeCreateSlotRequest(
    _Inout_ PCTRL_TRANSPORT_RUNTIME Runtime,
    _In_ CCHAR IrpStackSize,
    _In_ ULONG IoctlBufferSize,
    _Outptr_ PCTRL_ASYNC_SLOT_REQUEST* SlotRequest
)
{
    PCTRL_ASYNC_SLOT_REQUEST slotRequest;

    *SlotRequest = NULL;

    slotRequest = (PCTRL_ASYNC_SLOT_REQUEST)ControlAlloc(sizeof(*slotRequest));
    if (slotRequest == NULL) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    RtlZeroMemory(slotRequest, sizeof(*slotRequest));
    InitializeListHead(&slotRequest->PoolLink);
    slotRequest->Runtime = Runtime;
    slotRequest->IrpStackSize = IrpStackSize;

    slotRequest->Irp = IoAllocateIrp(IrpStackSize, FALSE);
    if (slotRequest->Irp == NULL) {
        ControlTransportRuntimeDestroySlotRequest(slotRequest);
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    slotRequest->IoctlBuffer = (PUCHAR)ControlAlloc(IoctlBufferSize);
    if (slotRequest->IoctlBuffer == NULL) {
        ControlTransportRuntimeDestroySlotRequest(slotRequest);
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    slotRequest->IoctlBufferSize = IoctlBufferSize;

    ControlTransportRuntimeResetSlotRequest(slotRequest);
    *SlotRequest = slotRequest;
    return STATUS_SUCCESS;
}

static
VOID
ControlTransportRuntimeFreeSlotPool(
    _Inout_ PCTRL_TRANSPORT_RUNTIME Runtime
)
{
    for (;;) {
        PLIST_ENTRY entry;
        PCTRL_ASYNC_SLOT_REQUEST slotRequest;

        WdfSpinLockAcquire(Runtime->FreeListLock);
        if (IsListEmpty(&Runtime->FreeList)) {
            WdfSpinLockRelease(Runtime->FreeListLock);
            break;
        }
        entry = RemoveHeadList(&Runtime->FreeList);
        WdfSpinLockRelease(Runtime->FreeListLock);

        slotRequest = CONTAINING_RECORD(entry, CTRL_ASYNC_SLOT_REQUEST, PoolLink);
        ControlTransportRuntimeDestroySlotRequest(slotRequest);
    }
}

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
    ControlTransportRuntimeFreeSlotPool(runtime);
    ControlTransportRuntimeDeleteLocks(runtime);
    ControlFree(runtime);
}

NTSTATUS
ControlTransportRuntimeAcquireSlotRequest(
    _Inout_ PCTRL_FILE_CONTEXT Context,
    _In_ CCHAR IrpStackSize,
    _In_ ULONG IoctlBufferSize,
    _Outptr_ PCTRL_ASYNC_SLOT_REQUEST* SlotRequest
)
{
    PCTRL_TRANSPORT_RUNTIME runtime;
    PCTRL_ASYNC_SLOT_REQUEST slotRequest;
    NTSTATUS status;
    LONG createdCount;

    *SlotRequest = NULL;

    if (Context == NULL || IrpStackSize <= 0 || IoctlBufferSize == 0) {
        return STATUS_INVALID_PARAMETER;
    }

    runtime = Context->TransportRuntime;
    if (runtime == NULL) {
        return STATUS_DEVICE_NOT_READY;
    }

    if (InterlockedCompareExchange(&runtime->State, 0, 0) != CtrlTransportRuntimeRunning) {
        InterlockedIncrement(&runtime->PoolAcquireFailures);
        return STATUS_DELETE_PENDING;
    }

    ControlTransportRuntimeReferenceActive(runtime);
    if (InterlockedCompareExchange(&runtime->State, 0, 0) != CtrlTransportRuntimeRunning) {
        ControlTransportRuntimeDereferenceActive(runtime);
        InterlockedIncrement(&runtime->PoolAcquireFailures);
        return STATUS_DELETE_PENDING;
    }

    slotRequest = NULL;
    WdfSpinLockAcquire(runtime->FreeListLock);
    if (!IsListEmpty(&runtime->FreeList)) {
        PLIST_ENTRY entry;

        entry = RemoveHeadList(&runtime->FreeList);
        slotRequest = CONTAINING_RECORD(entry, CTRL_ASYNC_SLOT_REQUEST, PoolLink);
    }
    WdfSpinLockRelease(runtime->FreeListLock);

    if (slotRequest == NULL) {
        createdCount = InterlockedIncrement(&runtime->PoolObjectCount);
        if (createdCount > CTRL_TRANSPORT_SLOT_POOL_HARD_LIMIT) {
            InterlockedDecrement(&runtime->PoolObjectCount);
            ControlTransportRuntimeDereferenceActive(runtime);
            InterlockedIncrement(&runtime->PoolAcquireFailures);
            return STATUS_DEVICE_BUSY;
        }

        status = ControlTransportRuntimeCreateSlotRequest(
            runtime,
            IrpStackSize,
            IoctlBufferSize,
            &slotRequest);
        if (!NT_SUCCESS(status)) {
            InterlockedDecrement(&runtime->PoolObjectCount);
            ControlTransportRuntimeDereferenceActive(runtime);
            InterlockedIncrement(&runtime->PoolAcquireFailures);
            return status;
        }

        ControlTransportRuntimeUpdateHighWater(runtime, createdCount);
    } else if (slotRequest->IrpStackSize < IrpStackSize ||
               slotRequest->IoctlBufferSize < IoctlBufferSize) {
        ControlTransportRuntimeReleaseSlotRequest(slotRequest);
        InterlockedIncrement(&runtime->PoolAcquireFailures);
        return STATUS_BUFFER_TOO_SMALL;
    }

    ControlTransportRuntimeResetSlotRequest(slotRequest);
    slotRequest->SessionContext = Context;
    *SlotRequest = slotRequest;
    return STATUS_SUCCESS;
}

VOID
ControlTransportRuntimeReleaseSlotRequest(
    _Inout_opt_ PCTRL_ASYNC_SLOT_REQUEST SlotRequest
)
{
    PCTRL_TRANSPORT_RUNTIME runtime;

    if (SlotRequest == NULL) {
        return;
    }

    runtime = SlotRequest->Runtime;
    if (runtime == NULL) {
        ControlTransportRuntimeDestroySlotRequest(SlotRequest);
        return;
    }

    if (SlotRequest->WorkItem != NULL) {
        IoFreeWorkItem(SlotRequest->WorkItem);
        SlotRequest->WorkItem = NULL;
    }

    ControlTransportRuntimeResetSlotRequest(SlotRequest);

    WdfSpinLockAcquire(runtime->FreeListLock);
    InsertTailList(&runtime->FreeList, &SlotRequest->PoolLink);
    WdfSpinLockRelease(runtime->FreeListLock);

    KeSetEvent(&runtime->FreeEvent, IO_NO_INCREMENT, FALSE);
    ControlTransportRuntimeDereferenceActive(runtime);
}
