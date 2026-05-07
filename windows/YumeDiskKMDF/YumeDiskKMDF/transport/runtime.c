#include "runtime.h"

#include "..\core\memory.h"

#define CTRL_TRANSPORT_SLOT_POOL_HARD_LIMIT 1024
#define CTRL_TRANSPORT_SYNC_POOL_HARD_LIMIT 256

_When_(Timeout == NULL, _IRQL_requires_max_(APC_LEVEL))
_When_(Timeout->QuadPart != 0, _IRQL_requires_max_(APC_LEVEL))
_When_(Timeout->QuadPart == 0, _IRQL_requires_max_(DISPATCH_LEVEL))
NTSYSAPI
NTSTATUS
NTAPI
ZwWaitForSingleObject(
    _In_ HANDLE Handle,
    _In_ BOOLEAN Alertable,
    _In_opt_ PLARGE_INTEGER Timeout
);

NTSYSAPI
NTSTATUS
NTAPI
ZwCreateEvent(
    _Out_ PHANDLE EventHandle,
    _In_ ACCESS_MASK DesiredAccess,
    _In_opt_ POBJECT_ATTRIBUTES ObjectAttributes,
    _In_ EVENT_TYPE EventType,
    _In_ BOOLEAN InitialState
);

NTSYSAPI
NTSTATUS
NTAPI
ZwResetEvent(
    _In_ HANDLE EventHandle,
    _Out_opt_ PLONG PreviousState
);

VOID
ControlTransportDispatchSlotRequest(
    _Inout_ PCTRL_ASYNC_SLOT_REQUEST SlotRequest
);

VOID
ControlTransportFailSlotRequest(
    _Inout_ PCTRL_ASYNC_SLOT_REQUEST SlotRequest,
    _In_ NTSTATUS Status
);

typedef enum _CTRL_TRANSPORT_RUNTIME_STATE {
    CtrlTransportRuntimeStopped = 0,
    CtrlTransportRuntimeRunning = 1,
    CtrlTransportRuntimeClosing = 2
} CTRL_TRANSPORT_RUNTIME_STATE;

struct _CTRL_TRANSPORT_RUNTIME {
    PCTRL_FILE_CONTEXT SessionContext;
    WDFSPINLOCK FreeListLock;
    WDFSPINLOCK SyncFreeListLock;
    WDFSPINLOCK SubmitQueueLock;
    LIST_ENTRY FreeList;
    LIST_ENTRY SyncFreeList;
    LIST_ENTRY SubmitQueue;
    KEVENT SubmitEvent;
    KEVENT ActiveZeroEvent;
    HANDLE WorkerThreadHandle;
    volatile LONG State;
    volatile LONG ActiveCount;
    volatile LONG PoolObjectCount;
    volatile LONG PoolHighWater;
    volatile LONG PoolAcquireFailures;
    volatile LONG SyncPoolObjectCount;
    volatile LONG SyncPoolHighWater;
    volatile LONG SyncPoolAcquireFailures;
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
ControlTransportRuntimeUpdateSyncHighWater(
    _Inout_ PCTRL_TRANSPORT_RUNTIME Runtime,
    _In_ LONG Value
)
{
    LONG oldValue;

    oldValue = InterlockedCompareExchange(&Runtime->SyncPoolHighWater, 0, 0);
    while (Value > oldValue) {
        LONG previousValue;

        previousValue = InterlockedCompareExchange(&Runtime->SyncPoolHighWater, Value, oldValue);
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

    if (SlotRequest->Irp != NULL) {
        IoFreeIrp(SlotRequest->Irp);
        SlotRequest->Irp = NULL;
    }
    ControlFree(SlotRequest->IoctlBuffer);
    SlotRequest->IoctlBuffer = NULL;
    ControlFree(SlotRequest);
}

static
VOID
ControlTransportRuntimeDestroySyncCommandBuffer(
    _Inout_opt_ PCTRL_SYNC_COMMAND_BUFFER CommandBuffer
)
{
    if (CommandBuffer == NULL) {
        return;
    }

    if (CommandBuffer->EventHandle != NULL) {
        ZwClose(CommandBuffer->EventHandle);
        CommandBuffer->EventHandle = NULL;
    }
    ControlFree(CommandBuffer->IoctlBuffer);
    CommandBuffer->IoctlBuffer = NULL;
    ControlFree(CommandBuffer);
}

static
NTSTATUS
ControlTransportRuntimeCreateSyncCommandBuffer(
    _Inout_ PCTRL_TRANSPORT_RUNTIME Runtime,
    _In_ ULONG IoctlBufferSize,
    _Outptr_ PCTRL_SYNC_COMMAND_BUFFER* CommandBuffer
)
{
    OBJECT_ATTRIBUTES eventAttributes;
    PCTRL_SYNC_COMMAND_BUFFER commandBuffer;
    NTSTATUS status;

    *CommandBuffer = NULL;

    commandBuffer = (PCTRL_SYNC_COMMAND_BUFFER)ControlAlloc(sizeof(*commandBuffer));
    if (commandBuffer == NULL) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    RtlZeroMemory(commandBuffer, sizeof(*commandBuffer));
    InitializeListHead(&commandBuffer->PoolLink);
    commandBuffer->Runtime = Runtime;

    commandBuffer->IoctlBuffer = (PUCHAR)ControlAlloc(IoctlBufferSize);
    if (commandBuffer->IoctlBuffer == NULL) {
        ControlTransportRuntimeDestroySyncCommandBuffer(commandBuffer);
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    commandBuffer->IoctlBufferSize = IoctlBufferSize;

    InitializeObjectAttributes(
        &eventAttributes,
        NULL,
        OBJ_KERNEL_HANDLE,
        NULL,
        NULL);
    status = ZwCreateEvent(
        &commandBuffer->EventHandle,
        SYNCHRONIZE | EVENT_MODIFY_STATE,
        &eventAttributes,
        NotificationEvent,
        FALSE);
    if (!NT_SUCCESS(status)) {
        ControlTransportRuntimeDestroySyncCommandBuffer(commandBuffer);
        return status;
    }

    *CommandBuffer = commandBuffer;
    return STATUS_SUCCESS;
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
    InitializeListHead(&slotRequest->SubmitLink);
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
ControlTransportRuntimeFreeSyncCommandPool(
    _Inout_ PCTRL_TRANSPORT_RUNTIME Runtime
)
{
    for (;;) {
        PLIST_ENTRY entry;
        PCTRL_SYNC_COMMAND_BUFFER commandBuffer;

        WdfSpinLockAcquire(Runtime->SyncFreeListLock);
        if (IsListEmpty(&Runtime->SyncFreeList)) {
            WdfSpinLockRelease(Runtime->SyncFreeListLock);
            break;
        }
        entry = RemoveHeadList(&Runtime->SyncFreeList);
        WdfSpinLockRelease(Runtime->SyncFreeListLock);

        commandBuffer = CONTAINING_RECORD(entry, CTRL_SYNC_COMMAND_BUFFER, PoolLink);
        ControlTransportRuntimeDestroySyncCommandBuffer(commandBuffer);
    }
}

static
BOOLEAN
ControlTransportRuntimeTakeSubmitQueue(
    _Inout_ PCTRL_TRANSPORT_RUNTIME Runtime,
    _Inout_ PLIST_ENTRY LocalList
)
{
    BOOLEAN hasItems;

    hasItems = FALSE;
    WdfSpinLockAcquire(Runtime->SubmitQueueLock);
    if (!IsListEmpty(&Runtime->SubmitQueue)) {
        PLIST_ENTRY first;
        PLIST_ENTRY last;

        first = Runtime->SubmitQueue.Flink;
        last = Runtime->SubmitQueue.Blink;

        LocalList->Flink = first;
        LocalList->Blink = last;
        first->Blink = LocalList;
        last->Flink = LocalList;

        InitializeListHead(&Runtime->SubmitQueue);
        hasItems = TRUE;
    }
    WdfSpinLockRelease(Runtime->SubmitQueueLock);
    return hasItems;
}

static
VOID
ControlTransportRuntimeFlushSubmitQueue(
    _Inout_ PCTRL_TRANSPORT_RUNTIME Runtime,
    _In_ NTSTATUS Status
)
{
    for (;;) {
        LIST_ENTRY localList;

        InitializeListHead(&localList);
        if (!ControlTransportRuntimeTakeSubmitQueue(Runtime, &localList)) {
            break;
        }

        while (!IsListEmpty(&localList)) {
            PLIST_ENTRY entry;
            PCTRL_ASYNC_SLOT_REQUEST slotRequest;

            entry = RemoveHeadList(&localList);
            slotRequest = CONTAINING_RECORD(entry, CTRL_ASYNC_SLOT_REQUEST, SubmitLink);
            InitializeListHead(&slotRequest->SubmitLink);
            ControlTransportFailSlotRequest(slotRequest, Status);
        }
    }
}

static
VOID
ControlTransportRuntimeSubmitWorker(
    _In_ PVOID StartContext
)
{
    PCTRL_TRANSPORT_RUNTIME runtime;

    runtime = (PCTRL_TRANSPORT_RUNTIME)StartContext;
    if (runtime == NULL) {
        PsTerminateSystemThread(STATUS_INVALID_PARAMETER);
    }

    for (;;) {
        LIST_ENTRY localList;

        if (InterlockedCompareExchange(&runtime->State, 0, 0) != CtrlTransportRuntimeRunning) {
            ControlTransportRuntimeFlushSubmitQueue(runtime, STATUS_DELETE_PENDING);
            break;
        }

        InitializeListHead(&localList);
        if (!ControlTransportRuntimeTakeSubmitQueue(runtime, &localList)) {
            (VOID)KeWaitForSingleObject(&runtime->SubmitEvent, Executive, KernelMode, FALSE, NULL);
            continue;
        }

        while (!IsListEmpty(&localList)) {
            PLIST_ENTRY entry;
            PCTRL_ASYNC_SLOT_REQUEST slotRequest;

            entry = RemoveHeadList(&localList);
            slotRequest = CONTAINING_RECORD(entry, CTRL_ASYNC_SLOT_REQUEST, SubmitLink);
            InitializeListHead(&slotRequest->SubmitLink);

            if (InterlockedCompareExchange(&runtime->State, 0, 0) != CtrlTransportRuntimeRunning) {
                ControlTransportFailSlotRequest(slotRequest, STATUS_DELETE_PENDING);
                continue;
            }

            ControlTransportDispatchSlotRequest(slotRequest);
        }
    }

    PsTerminateSystemThread(STATUS_SUCCESS);
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

    if (Runtime->SyncFreeListLock != NULL) {
        WdfObjectDelete(Runtime->SyncFreeListLock);
        Runtime->SyncFreeListLock = NULL;
    }

    if (Runtime->FreeListLock != NULL) {
        WdfObjectDelete(Runtime->FreeListLock);
        Runtime->FreeListLock = NULL;
    }
}

static
VOID
ControlTransportRuntimeStopWorker(
    _Inout_ PCTRL_TRANSPORT_RUNTIME Runtime
)
{
    HANDLE workerThreadHandle;

    InterlockedExchange(&Runtime->State, CtrlTransportRuntimeClosing);
    KeSetEvent(&Runtime->SubmitEvent, IO_NO_INCREMENT, FALSE);

    workerThreadHandle = Runtime->WorkerThreadHandle;
    Runtime->WorkerThreadHandle = NULL;
    if (workerThreadHandle != NULL) {
        (VOID)ZwWaitForSingleObject(workerThreadHandle, FALSE, NULL);
        ZwClose(workerThreadHandle);
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
    OBJECT_ATTRIBUTES threadAttributes;
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
    InitializeListHead(&runtime->SyncFreeList);
    InitializeListHead(&runtime->SubmitQueue);
    KeInitializeEvent(&runtime->SubmitEvent, SynchronizationEvent, FALSE);
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
    status = WdfSpinLockCreate(&attributes, &runtime->SyncFreeListLock);
    if (!NT_SUCCESS(status)) {
        ControlTransportRuntimeDeleteLocks(runtime);
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

    InitializeObjectAttributes(&threadAttributes, NULL, OBJ_KERNEL_HANDLE, NULL, NULL);
    status = PsCreateSystemThread(
        &runtime->WorkerThreadHandle,
        THREAD_ALL_ACCESS,
        &threadAttributes,
        NULL,
        NULL,
        ControlTransportRuntimeSubmitWorker,
        runtime);
    if (!NT_SUCCESS(status)) {
        ControlTransportRuntimeDeleteLocks(runtime);
        ControlFree(runtime);
        return status;
    }

    if (InterlockedCompareExchangePointer((PVOID volatile*)&Context->TransportRuntime, runtime, NULL) != NULL) {
        ControlTransportRuntimeStopWorker(runtime);
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
    KeSetEvent(&runtime->SubmitEvent, IO_NO_INCREMENT, FALSE);
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
    ControlTransportRuntimeStopWorker(runtime);
    ControlTransportRuntimeDrainActive(runtime);
    InterlockedExchange(&runtime->State, CtrlTransportRuntimeStopped);
    ControlTransportRuntimeFreeSlotPool(runtime);
    ControlTransportRuntimeFreeSyncCommandPool(runtime);
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

    ControlTransportRuntimeResetSlotRequest(SlotRequest);

    WdfSpinLockAcquire(runtime->FreeListLock);
    InsertTailList(&runtime->FreeList, &SlotRequest->PoolLink);
    WdfSpinLockRelease(runtime->FreeListLock);

    ControlTransportRuntimeDereferenceActive(runtime);
}

NTSTATUS
ControlTransportRuntimeSubmitSlotRequest(
    _Inout_ PCTRL_ASYNC_SLOT_REQUEST SlotRequest
)
{
    PCTRL_TRANSPORT_RUNTIME runtime;

    if (SlotRequest == NULL || SlotRequest->Runtime == NULL) {
        return STATUS_INVALID_PARAMETER;
    }

    runtime = SlotRequest->Runtime;
    if (InterlockedCompareExchange(&runtime->State, 0, 0) != CtrlTransportRuntimeRunning) {
        return STATUS_DELETE_PENDING;
    }

    WdfSpinLockAcquire(runtime->SubmitQueueLock);
    if (InterlockedCompareExchange(&runtime->State, 0, 0) != CtrlTransportRuntimeRunning) {
        WdfSpinLockRelease(runtime->SubmitQueueLock);
        return STATUS_DELETE_PENDING;
    }

    InsertTailList(&runtime->SubmitQueue, &SlotRequest->SubmitLink);
    InterlockedIncrement(&runtime->SubmitQueued);
    WdfSpinLockRelease(runtime->SubmitQueueLock);

    KeSetEvent(&runtime->SubmitEvent, IO_NO_INCREMENT, FALSE);
    return STATUS_PENDING;
}

NTSTATUS
ControlTransportRuntimeAcquireSyncCommandBuffer(
    _Inout_ PCTRL_FILE_CONTEXT Context,
    _In_ ULONG IoctlBufferSize,
    _Outptr_ PCTRL_SYNC_COMMAND_BUFFER* CommandBuffer
)
{
    PCTRL_TRANSPORT_RUNTIME runtime;
    PCTRL_SYNC_COMMAND_BUFFER commandBuffer;
    NTSTATUS status;
    LONG createdCount;

    *CommandBuffer = NULL;

    if (Context == NULL || IoctlBufferSize == 0) {
        return STATUS_INVALID_PARAMETER;
    }

    runtime = Context->TransportRuntime;
    if (runtime == NULL) {
        return STATUS_DEVICE_NOT_READY;
    }

    if (InterlockedCompareExchange(&runtime->State, 0, 0) != CtrlTransportRuntimeRunning) {
        InterlockedIncrement(&runtime->SyncPoolAcquireFailures);
        return STATUS_DELETE_PENDING;
    }

    ControlTransportRuntimeReferenceActive(runtime);
    if (InterlockedCompareExchange(&runtime->State, 0, 0) != CtrlTransportRuntimeRunning) {
        ControlTransportRuntimeDereferenceActive(runtime);
        InterlockedIncrement(&runtime->SyncPoolAcquireFailures);
        return STATUS_DELETE_PENDING;
    }

    commandBuffer = NULL;
    WdfSpinLockAcquire(runtime->SyncFreeListLock);
    if (!IsListEmpty(&runtime->SyncFreeList)) {
        PLIST_ENTRY entry;

        entry = RemoveHeadList(&runtime->SyncFreeList);
        commandBuffer = CONTAINING_RECORD(entry, CTRL_SYNC_COMMAND_BUFFER, PoolLink);
    }
    WdfSpinLockRelease(runtime->SyncFreeListLock);

    if (commandBuffer == NULL) {
        createdCount = InterlockedIncrement(&runtime->SyncPoolObjectCount);
        if (createdCount > CTRL_TRANSPORT_SYNC_POOL_HARD_LIMIT) {
            InterlockedDecrement(&runtime->SyncPoolObjectCount);
            ControlTransportRuntimeDereferenceActive(runtime);
            InterlockedIncrement(&runtime->SyncPoolAcquireFailures);
            return STATUS_DEVICE_BUSY;
        }

        status = ControlTransportRuntimeCreateSyncCommandBuffer(
            runtime,
            IoctlBufferSize,
            &commandBuffer);
        if (!NT_SUCCESS(status)) {
            InterlockedDecrement(&runtime->SyncPoolObjectCount);
            ControlTransportRuntimeDereferenceActive(runtime);
            InterlockedIncrement(&runtime->SyncPoolAcquireFailures);
            return status;
        }

        ControlTransportRuntimeUpdateSyncHighWater(runtime, createdCount);
    } else if (commandBuffer->IoctlBufferSize < IoctlBufferSize) {
        ControlTransportRuntimeDestroySyncCommandBuffer(commandBuffer);
        commandBuffer = NULL;

        status = ControlTransportRuntimeCreateSyncCommandBuffer(
            runtime,
            IoctlBufferSize,
            &commandBuffer);
        if (!NT_SUCCESS(status)) {
            InterlockedDecrement(&runtime->SyncPoolObjectCount);
            ControlTransportRuntimeDereferenceActive(runtime);
            InterlockedIncrement(&runtime->SyncPoolAcquireFailures);
            return status;
        }
    }

    RtlZeroMemory(commandBuffer->IoctlBuffer, commandBuffer->IoctlBufferSize);
    (VOID)ZwResetEvent(commandBuffer->EventHandle, NULL);
    *CommandBuffer = commandBuffer;
    return STATUS_SUCCESS;
}

VOID
ControlTransportRuntimeReleaseSyncCommandBuffer(
    _Inout_opt_ PCTRL_SYNC_COMMAND_BUFFER CommandBuffer
)
{
    PCTRL_TRANSPORT_RUNTIME runtime;

    if (CommandBuffer == NULL) {
        return;
    }

    runtime = CommandBuffer->Runtime;
    if (runtime == NULL) {
        ControlTransportRuntimeDestroySyncCommandBuffer(CommandBuffer);
        return;
    }

    WdfSpinLockAcquire(runtime->SyncFreeListLock);
    InsertTailList(&runtime->SyncFreeList, &CommandBuffer->PoolLink);
    WdfSpinLockRelease(runtime->SyncFreeListLock);

    ControlTransportRuntimeDereferenceActive(runtime);
}
