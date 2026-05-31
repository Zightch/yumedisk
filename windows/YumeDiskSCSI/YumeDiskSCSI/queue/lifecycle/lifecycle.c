#include "lifecycle.h"

#include "..\\event\\event.h"
#include "..\\read\\read.h"
#include "..\\slot\\slot.h"
#include "..\\write\\write.h"
#include "..\\..\\core\\memory.h"

static
VOID
DiskResetQueueTargetState(
    _Inout_ PYUME_DISK Disk
)
{
    InitializeListHead(&Disk->Queue.PostedReadSlots);
    InitializeListHead(&Disk->Queue.PendingReads);
    InitializeListHead(&Disk->Queue.PostedWriteSlots);
    InitializeListHead(&Disk->Queue.PendingWrites);
    Disk->Queue.PostedReadSlotCount = 0;
    Disk->Queue.PendingReadCount = 0;
    Disk->Queue.PendingReadIssuedCount = 0;
    Disk->Queue.PostedWriteSlotCount = 0;
    Disk->Queue.PendingWriteCount = 0;
    Disk->Queue.WriteSlotPayloadBytes = 0;
    Disk->EventSlot.PendingSrb = NULL;
    Disk->EventSlot.SlotId = 0ull;
    Disk->EventSlot.KernelVa = 0ull;
    Disk->EventSlot.Capacity = 0u;
    Disk->EventSlot.Flags = 0u;
}

static
VOID
DiskCompleteTargetPendingInternal(
    _In_ PVOID DeviceExtension,
    _In_ ULONG TargetId,
    _In_ NTSTATUS IoStatus,
    _In_ NTSTATUS EventSlotStatus,
    _In_opt_ const YUMEDISK_DISK_EVENT* EventRecord
)
{
    PDEVICE_CONTEXT extension;
    PYUME_DISK disk;
    LIST_ENTRY readRequests;
    LIST_ENTRY readSlots;
    LIST_ENTRY writeRequests;
    LIST_ENTRY writeSlots;
    KIRQL oldIrql;

    extension = (PDEVICE_CONTEXT)DeviceExtension;
    if (TargetId >= extension->MaxTargets) {
        return;
    }

    disk = &extension->Disk[TargetId];
    InitializeListHead(&readRequests);
    InitializeListHead(&readSlots);
    InitializeListHead(&writeRequests);
    InitializeListHead(&writeSlots);

    KeAcquireSpinLock(&disk->Queue.ReadQueueLock, &oldIrql);
    if (!IsListEmpty(&disk->Queue.PendingReads)) {
        readRequests = disk->Queue.PendingReads;
        readRequests.Blink->Flink = &readRequests;
        readRequests.Flink->Blink = &readRequests;
        InitializeListHead(&disk->Queue.PendingReads);
    }
    if (!IsListEmpty(&disk->Queue.PostedReadSlots)) {
        readSlots = disk->Queue.PostedReadSlots;
        readSlots.Blink->Flink = &readSlots;
        readSlots.Flink->Blink = &readSlots;
        InitializeListHead(&disk->Queue.PostedReadSlots);
    }
    disk->Queue.PendingReadCount = 0;
    disk->Queue.PendingReadIssuedCount = 0;
    disk->Queue.PostedReadSlotCount = 0;
    KeReleaseSpinLock(&disk->Queue.ReadQueueLock, oldIrql);

    KeAcquireSpinLock(&disk->Queue.WriteQueueLock, &oldIrql);
    if (!IsListEmpty(&disk->Queue.PendingWrites)) {
        writeRequests = disk->Queue.PendingWrites;
        writeRequests.Blink->Flink = &writeRequests;
        writeRequests.Flink->Blink = &writeRequests;
        InitializeListHead(&disk->Queue.PendingWrites);
    }
    if (!IsListEmpty(&disk->Queue.PostedWriteSlots)) {
        writeSlots = disk->Queue.PostedWriteSlots;
        writeSlots.Blink->Flink = &writeSlots;
        writeSlots.Flink->Blink = &writeSlots;
        InitializeListHead(&disk->Queue.PostedWriteSlots);
    }
    disk->Queue.PendingWriteCount = 0;
    disk->Queue.PostedWriteSlotCount = 0;
    disk->Queue.WriteSlotPayloadBytes = 0;
    KeReleaseSpinLock(&disk->Queue.WriteQueueLock, oldIrql);

    while (!IsListEmpty(&readRequests)) {
        PYUME_READ_REQUEST request;

        request = CONTAINING_RECORD(RemoveHeadList(&readRequests), YUME_READ_REQUEST, Link);
        DiskCompleteReadRequest(DeviceExtension, request, IoStatus);
    }

    while (!IsListEmpty(&readSlots)) {
        PYUME_POSTED_SLOT slot;

        slot = CONTAINING_RECORD(RemoveHeadList(&readSlots), YUME_POSTED_SLOT, Link);
        DiskCompleteSlotSrb(DeviceExtension, slot, IoStatus);
        DiskFree(slot);
    }

    while (!IsListEmpty(&writeRequests)) {
        PYUME_WRITE_REQUEST request;

        request = CONTAINING_RECORD(RemoveHeadList(&writeRequests), YUME_WRITE_REQUEST, Link);
        request->FinalStatus = IoStatus;
        DiskCompleteWriteRequest(DeviceExtension, request);
    }

    while (!IsListEmpty(&writeSlots)) {
        PYUME_POSTED_SLOT slot;

        slot = CONTAINING_RECORD(RemoveHeadList(&writeSlots), YUME_POSTED_SLOT, Link);
        DiskCompleteSlotSrb(DeviceExtension, slot, IoStatus);
        DiskFree(slot);
    }

    DiskCompletePendingEventSlot(DeviceExtension, TargetId, EventSlotStatus, EventRecord);
}

VOID
DiskInitializeQueueState(
    _Out_ PDEVICE_CONTEXT Extension
)
{
    ULONG index;

    Extension->NextEventId = 0;
    Extension->DebugProgressCounter = 0;
    Extension->DebugReadRequestsQueued = 0;
    Extension->DebugReadSlotsIssued = 0;
    Extension->DebugReadAcksApplied = 0;
    Extension->DebugReadRequestsCompleted = 0;
    Extension->DebugReadRequestsFailed = 0;
    Extension->DebugWriteRequestsQueued = 0;
    Extension->DebugWriteFragmentsIssued = 0;
    Extension->DebugWriteAcksApplied = 0;
    Extension->DebugWriteRequestsCompleted = 0;
    Extension->DebugWriteRequestsFailed = 0;

    for (index = 0; index < Extension->MaxTargets; ++index) {
        DiskResetQueueTargetState(&Extension->Disk[index]);
    }
}

VOID
DiskFreeQueuedState(
    _In_ PVOID DeviceExtension
)
{
    PDEVICE_CONTEXT extension;
    ULONG index;

    extension = (PDEVICE_CONTEXT)DeviceExtension;
    for (index = 0; index < extension->MaxTargets; ++index) {
        DiskResetQueueTargetState(&extension->Disk[index]);
    }
}

VOID
DiskCompleteTargetPending(
    _In_ PVOID DeviceExtension,
    _In_ ULONG TargetId,
    _In_ NTSTATUS Status
)
{
    DiskCompleteTargetPendingInternal(
        DeviceExtension,
        TargetId,
        Status,
        Status,
        NULL);
}

VOID
DiskCompleteTargetPendingWithEvent(
    _In_ PVOID DeviceExtension,
    _In_ ULONG TargetId,
    _In_ NTSTATUS IoStatus,
    _In_ const YUMEDISK_DISK_EVENT* EventRecord
)
{
    DiskCompleteTargetPendingInternal(
        DeviceExtension,
        TargetId,
        IoStatus,
        STATUS_SUCCESS,
        EventRecord);
}

VOID
DiskCompleteAllPending(
    _In_ PVOID DeviceExtension,
    _In_ NTSTATUS Status
)
{
    PDEVICE_CONTEXT extension;
    ULONG index;

    extension = (PDEVICE_CONTEXT)DeviceExtension;
    for (index = 0; index < extension->MaxTargets; ++index) {
        DiskCompleteTargetPending(DeviceExtension, index, Status);
    }
}

VOID
DiskCompleteTargetPendingIo(
    _In_ PVOID DeviceExtension,
    _In_ ULONG TargetId,
    _In_ NTSTATUS Status
)
{
    DiskCompleteTargetPending(DeviceExtension, TargetId, Status);
}

VOID
DiskCompleteAllPendingIo(
    _In_ PVOID DeviceExtension,
    _In_ NTSTATUS Status
)
{
    DiskCompleteAllPending(DeviceExtension, Status);
}
