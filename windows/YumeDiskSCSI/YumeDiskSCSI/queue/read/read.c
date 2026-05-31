#include "read.h"

#include "..\internal.h"
#include "..\slot\slot.h"
#include "..\..\core\memory.h"

static
PYUME_READ_REQUEST
DiskAllocReadRequest(
    _In_ PSTORAGE_REQUEST_BLOCK Srb,
    _In_ UINT64 EventId,
    _In_ UINT64 Lba,
    _In_ ULONG BlockCount,
    _In_ ULONG DataLength
)
{
    PYUME_READ_REQUEST request;

    request = (PYUME_READ_REQUEST)DiskAlloc(sizeof(*request));
    if (request == NULL) {
        return NULL;
    }

    RtlZeroMemory(request, sizeof(*request));
    request->Srb = Srb;
    request->EventId = EventId;
    request->Lba = Lba;
    request->BlockCount = BlockCount;
    request->DataLength = DataLength;
    return request;
}

static
PYUME_READ_REQUEST
DiskFindNextPendingReadLocked(
    _In_ const PYUME_DISK_QUEUE_STATE Queue
)
{
    PLIST_ENTRY entry;

    for (entry = Queue->PendingReads.Flink;
         entry != &Queue->PendingReads;
         entry = entry->Flink) {
        PYUME_READ_REQUEST request;

        request = CONTAINING_RECORD(entry, YUME_READ_REQUEST, Link);
        if (!request->SlotIssued) {
            return request;
        }
    }

    return NULL;
}

static
PYUME_READ_REQUEST
DiskFindReadRequestByEventIdLocked(
    _In_ const PYUME_DISK_QUEUE_STATE Queue,
    _In_ UINT64 EventId
)
{
    PLIST_ENTRY entry;

    for (entry = Queue->PendingReads.Flink;
         entry != &Queue->PendingReads;
         entry = entry->Flink) {
        PYUME_READ_REQUEST request;

        request = CONTAINING_RECORD(entry, YUME_READ_REQUEST, Link);
        if (request->EventId == EventId) {
            return request;
        }
    }

    return NULL;
}

VOID
DiskCompleteReadRequest(
    _In_ PVOID DeviceExtension,
    _In_ PYUME_READ_REQUEST Request,
    _In_ NTSTATUS Status
)
{
    if (NT_SUCCESS(Status)) {
        InterlockedIncrement64(&((PDEVICE_CONTEXT)DeviceExtension)->DebugReadRequestsCompleted);
        DiskCompleteScsiSrb(DeviceExtension, Request->Srb, STATUS_SUCCESS, Request->DataLength);
    } else {
        InterlockedIncrement64(&((PDEVICE_CONTEXT)DeviceExtension)->DebugReadRequestsFailed);
        DiskCompleteScsiSrb(DeviceExtension, Request->Srb, Status, 0);
    }

    DiskTickProgress((PDEVICE_CONTEXT)DeviceExtension);
    DiskFree(Request);
}

VOID
DiskDrainReadSlots(
    _In_ PVOID DeviceExtension,
    _In_ UCHAR TargetId
)
{
    PDEVICE_CONTEXT extension;
    PYUME_DISK disk;

    extension = (PDEVICE_CONTEXT)DeviceExtension;
    disk = &extension->Disk[TargetId];

    for (;;) {
        KIRQL oldIrql;
        PYUME_DISK_QUEUE_STATE queue;
        PYUME_READ_REQUEST request;
        PYUME_POSTED_SLOT slot;

        queue = &disk->Queue;
        KeAcquireSpinLock(&queue->ReadQueueLock, &oldIrql);
        request = DiskFindNextPendingReadLocked(queue);
        if (request == NULL || IsListEmpty(&queue->PostedReadSlots)) {
            KeReleaseSpinLock(&queue->ReadQueueLock, oldIrql);
            break;
        }

        slot = CONTAINING_RECORD(RemoveHeadList(&queue->PostedReadSlots), YUME_POSTED_SLOT, Link);
        queue->PostedReadSlotCount--;
        request->SlotIssued = TRUE;
        queue->PendingReadIssuedCount++;
        KeReleaseSpinLock(&queue->ReadQueueLock, oldIrql);

        if (NT_SUCCESS(DiskWriteReadSlotEvent(
                request->EventId,
                request->Lba,
                request->BlockCount,
                request->DataLength,
                slot))) {
            InterlockedIncrement64(&extension->DebugReadSlotsIssued);
            DiskTickProgress(extension);
            DiskCompleteSlotSrb(DeviceExtension, slot, STATUS_SUCCESS);
        } else {
            DiskCompleteSlotSrb(DeviceExtension, slot, STATUS_BUFFER_TOO_SMALL);
        }

        DiskFree(slot);
    }
}

NTSTATUS
DiskQueueReadSrb(
    _In_ PVOID DeviceExtension,
    _In_ PSTORAGE_REQUEST_BLOCK Srb,
    _In_ UCHAR TargetId,
    _In_ UINT64 Lba,
    _In_ ULONG BlockCount,
    _In_ ULONG DataLength
)
{
    PDEVICE_CONTEXT extension;
    PYUME_DISK disk;
    PYUME_READ_REQUEST request;
    KIRQL oldIrql;

    extension = (PDEVICE_CONTEXT)DeviceExtension;
    if (TargetId >= extension->MaxTargets || !DiskIsUsableTargetId(TargetId)) {
        return STATUS_INVALID_PARAMETER;
    }

    disk = &extension->Disk[TargetId];
    if (!disk->Present || !disk->Configured) {
        return STATUS_DEVICE_NOT_CONNECTED;
    }

    request = DiskAllocReadRequest(Srb, DiskNextEventId(extension), Lba, BlockCount, DataLength);
    if (request == NULL) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    KeAcquireSpinLock(&disk->Queue.ReadQueueLock, &oldIrql);
    InsertTailList(&disk->Queue.PendingReads, &request->Link);
    disk->Queue.PendingReadCount++;
    KeReleaseSpinLock(&disk->Queue.ReadQueueLock, oldIrql);

    InterlockedIncrement64(&extension->DebugReadRequestsQueued);
    DiskTickProgress(extension);
    DiskDrainReadSlots(DeviceExtension, TargetId);
    return STATUS_PENDING;
}

NTSTATUS
DiskHandleReadAckIoctl(
    _In_ PVOID DeviceExtension,
    _Inout_ PYUMEDISK_MESSAGE Message
)
{
    PDEVICE_CONTEXT extension;
    PYUMEDISK_READ_ACK readAck;
    PYUME_DISK disk;
    PYUME_READ_REQUEST request;
    KIRQL oldIrql;
    NTSTATUS completionStatus;

    extension = (PDEVICE_CONTEXT)DeviceExtension;
    if (Message->Header.PayloadLength < sizeof(*readAck) ||
        Message->Header.TargetId >= extension->MaxTargets ||
        !DiskIsUsableTargetId(Message->Header.TargetId)) {
        return STATUS_INVALID_PARAMETER;
    }

    disk = &extension->Disk[Message->Header.TargetId];
    readAck = (PYUMEDISK_READ_ACK)Message->Payload;

    KeAcquireSpinLock(&disk->Queue.ReadQueueLock, &oldIrql);
    request = DiskFindReadRequestByEventIdLocked(&disk->Queue, readAck->EventId);
    if (request == NULL) {
        KeReleaseSpinLock(&disk->Queue.ReadQueueLock, oldIrql);
        return STATUS_NOT_FOUND;
    }

    RemoveEntryList(&request->Link);
    disk->Queue.PendingReadCount--;
    if (request->SlotIssued && disk->Queue.PendingReadIssuedCount != 0) {
        disk->Queue.PendingReadIssuedCount--;
    }
    KeReleaseSpinLock(&disk->Queue.ReadQueueLock, oldIrql);

    completionStatus = readAck->IoStatus;
    if (NT_SUCCESS(completionStatus)) {
        if (readAck->DataLength != request->DataLength || readAck->KernelVa == 0) {
            DiskCompleteReadRequest(DeviceExtension, request, STATUS_INVALID_PARAMETER);
            return STATUS_INVALID_PARAMETER;
        }

        RtlCopyMemory(
            request->Srb->DataBuffer,
            (PVOID)(ULONG_PTR)readAck->KernelVa,
            readAck->DataLength);
        InterlockedIncrement64(&extension->DebugReadAcksApplied);
        DiskCompleteReadRequest(DeviceExtension, request, STATUS_SUCCESS);
    } else {
        DiskCompleteReadRequest(DeviceExtension, request, completionStatus);
    }

    DiskInitMessageStatus(Message, YumeDiskCommandReadAck, STATUS_SUCCESS, 0);
    return STATUS_SUCCESS;
}
