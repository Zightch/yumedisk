#include "queue.h"
#include "event/event.h"
#include "internal.h"
#include "lifecycle/lifecycle.h"
#include "read/read.h"
#include "slot/slot.h"
#include "write/write.h"

#include "..\\core\\memory.h"

NTSTATUS
DiskHandleSubmitSlotIoctl(
    _In_ PVOID DeviceExtension,
    _In_ PSTORAGE_REQUEST_BLOCK Srb,
    _Inout_ PYUMEDISK_MESSAGE Message
)
{
    PDEVICE_CONTEXT extension;
    PYUMEDISK_SUBMIT_SLOT submitSlot;
    YUMEDISK_SLOT_DESCRIPTOR* slotDescriptor;
    PYUME_DISK disk;
    PYUME_POSTED_SLOT postedSlot;
    KIRQL oldIrql;
    ULONG writePayloadBytes;

    extension = (PDEVICE_CONTEXT)DeviceExtension;
    if (Message->Header.PayloadLength < sizeof(*submitSlot)) {
        return STATUS_INVALID_PARAMETER;
    }

    submitSlot = (PYUMEDISK_SUBMIT_SLOT)Message->Payload;
    slotDescriptor = &submitSlot->Slot;
    if (slotDescriptor->SessionId != Message->Header.SessionId ||
        slotDescriptor->TargetId != Message->Header.TargetId ||
        slotDescriptor->KernelVa == 0 ||
        slotDescriptor->TargetId >= extension->MaxTargets ||
        !DiskIsUsableTargetId(slotDescriptor->TargetId)) {
        return STATUS_INVALID_PARAMETER;
    }

    disk = &extension->Disk[slotDescriptor->TargetId];
    if (!disk->Present || !disk->Configured) {
        return STATUS_DEVICE_NOT_CONNECTED;
    }

    if (slotDescriptor->SlotType == YumeDiskSlotTypeRead) {
        if (slotDescriptor->Capacity < sizeof(YUMEDISK_READ_SLOT_EVENT)) {
            return STATUS_BUFFER_TOO_SMALL;
        }
    } else if (slotDescriptor->SlotType == YumeDiskSlotTypeWrite) {
        writePayloadBytes = DiskComputeWritePayloadBytes(disk->SectorSize, slotDescriptor->Capacity);
        if (writePayloadBytes == 0) {
            return STATUS_BUFFER_TOO_SMALL;
        }
    } else {
        return STATUS_INVALID_PARAMETER;
    }

    postedSlot = DiskAllocPostedSlot(Srb, slotDescriptor);
    if (postedSlot == NULL) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    if (slotDescriptor->SlotType == YumeDiskSlotTypeRead) {
        KeAcquireSpinLock(&disk->Queue.ReadQueueLock, &oldIrql);
        InsertTailList(&disk->Queue.PostedReadSlots, &postedSlot->Link);
        disk->Queue.PostedReadSlotCount++;
        KeReleaseSpinLock(&disk->Queue.ReadQueueLock, oldIrql);
        DiskTickProgress(extension);
        DiskDrainReadSlots(DeviceExtension, (UCHAR)slotDescriptor->TargetId);
        return STATUS_PENDING;
    }

    writePayloadBytes = DiskComputeWritePayloadBytes(disk->SectorSize, slotDescriptor->Capacity);
    KeAcquireSpinLock(&disk->Queue.WriteQueueLock, &oldIrql);
    if (disk->Queue.WriteSlotPayloadBytes == 0) {
        disk->Queue.WriteSlotPayloadBytes = writePayloadBytes;
    } else if (disk->Queue.WriteSlotPayloadBytes != writePayloadBytes) {
        KeReleaseSpinLock(&disk->Queue.WriteQueueLock, oldIrql);
        DiskFree(postedSlot);
        return STATUS_INVALID_PARAMETER;
    }

    InsertTailList(&disk->Queue.PostedWriteSlots, &postedSlot->Link);
    disk->Queue.PostedWriteSlotCount++;
    KeReleaseSpinLock(&disk->Queue.WriteQueueLock, oldIrql);

    DiskTickProgress(extension);
    DiskDrainWriteSlots(DeviceExtension, (UCHAR)slotDescriptor->TargetId);
    return STATUS_PENDING;
}

NTSTATUS
DiskHandleCancelSlotIoctl(
    _In_ PVOID DeviceExtension,
    _Inout_ PYUMEDISK_MESSAGE Message
)
{
    PDEVICE_CONTEXT extension;
    PYUMEDISK_CANCEL_SLOT cancelSlot;
    PYUME_DISK disk;
    PYUME_POSTED_SLOT removedSlot;
    KIRQL oldIrql;

    extension = (PDEVICE_CONTEXT)DeviceExtension;
    if (Message->Header.PayloadLength < sizeof(*cancelSlot)) {
        return STATUS_INVALID_PARAMETER;
    }

    cancelSlot = (PYUMEDISK_CANCEL_SLOT)Message->Payload;
    if (cancelSlot->TargetId != Message->Header.TargetId ||
        cancelSlot->TargetId >= extension->MaxTargets ||
        !DiskIsUsableTargetId(cancelSlot->TargetId)) {
        return STATUS_INVALID_PARAMETER;
    }

    disk = &extension->Disk[cancelSlot->TargetId];
    removedSlot = NULL;

    if (cancelSlot->SlotType == YumeDiskSlotTypeRead) {
        KeAcquireSpinLock(&disk->Queue.ReadQueueLock, &oldIrql);
        removedSlot = DiskRemovePostedSlotByIdLocked(
            &disk->Queue.PostedReadSlots,
            cancelSlot->SlotId);
        if (removedSlot != NULL) {
            disk->Queue.PostedReadSlotCount--;
        }
        KeReleaseSpinLock(&disk->Queue.ReadQueueLock, oldIrql);
    } else if (cancelSlot->SlotType == YumeDiskSlotTypeWrite) {
        KeAcquireSpinLock(&disk->Queue.WriteQueueLock, &oldIrql);
        removedSlot = DiskRemovePostedSlotByIdLocked(
            &disk->Queue.PostedWriteSlots,
            cancelSlot->SlotId);
        if (removedSlot != NULL) {
            disk->Queue.PostedWriteSlotCount--;
            DiskResetWriteSlotShapeLocked(&disk->Queue);
        }
        KeReleaseSpinLock(&disk->Queue.WriteQueueLock, oldIrql);
    } else {
        return STATUS_INVALID_PARAMETER;
    }

    if (removedSlot == NULL) {
        return STATUS_NOT_FOUND;
    }

    DiskCompleteSlotSrb(DeviceExtension, removedSlot, STATUS_CANCELLED);
    DiskFree(removedSlot);
    DiskInitMessageStatus(Message, YumeDiskCommandCancelSlot, STATUS_SUCCESS, 0);
    return STATUS_SUCCESS;
}

NTSTATUS
DiskQueryDebugState(
    _In_ PVOID DeviceExtension,
    _Out_ PYUMEDISK_DEBUG_STATE DebugState
)
{
    PDEVICE_CONTEXT extension;
    ULONG index;
    KIRQL oldIrql;

    extension = (PDEVICE_CONTEXT)DeviceExtension;
    RtlZeroMemory(DebugState, sizeof(*DebugState));

    KeAcquireSpinLock(&extension->SessionLock, &oldIrql);
    DebugState->ActiveSessionId = extension->CurrentSessionId;
    KeReleaseSpinLock(&extension->SessionLock, oldIrql);
    DebugState->ProgressCounter = InterlockedCompareExchange64(&extension->DebugProgressCounter, 0, 0);
    DebugState->ReadRequestsQueued = InterlockedCompareExchange64(&extension->DebugReadRequestsQueued, 0, 0);
    DebugState->ReadSlotsIssued = InterlockedCompareExchange64(&extension->DebugReadSlotsIssued, 0, 0);
    DebugState->ReadAcksApplied = InterlockedCompareExchange64(&extension->DebugReadAcksApplied, 0, 0);
    DebugState->ReadRequestsCompleted = InterlockedCompareExchange64(&extension->DebugReadRequestsCompleted, 0, 0);
    DebugState->ReadRequestsFailed = InterlockedCompareExchange64(&extension->DebugReadRequestsFailed, 0, 0);
    DebugState->WriteRequestsQueued = InterlockedCompareExchange64(&extension->DebugWriteRequestsQueued, 0, 0);
    DebugState->WriteFragmentsIssued = InterlockedCompareExchange64(&extension->DebugWriteFragmentsIssued, 0, 0);
    DebugState->WriteAcksApplied = InterlockedCompareExchange64(&extension->DebugWriteAcksApplied, 0, 0);
    DebugState->WriteRequestsCompleted = InterlockedCompareExchange64(&extension->DebugWriteRequestsCompleted, 0, 0);
    DebugState->WriteRequestsFailed = InterlockedCompareExchange64(&extension->DebugWriteRequestsFailed, 0, 0);

    for (index = 0; index < extension->MaxTargets; ++index) {
        PYUME_DISK_QUEUE_STATE queue;

        queue = &extension->Disk[index].Queue;

        KeAcquireSpinLock(&queue->ReadQueueLock, &oldIrql);
        DebugState->PostedReadSlots += queue->PostedReadSlotCount;
        DebugState->PendingReads += queue->PendingReadCount;
        DebugState->PendingReadsIssued += queue->PendingReadIssuedCount;
        KeReleaseSpinLock(&queue->ReadQueueLock, oldIrql);

        KeAcquireSpinLock(&queue->WriteQueueLock, &oldIrql);
        {
            PLIST_ENTRY entry;

            DebugState->PostedWriteSlots += queue->PostedWriteSlotCount;
            DebugState->PendingWrites += queue->PendingWriteCount;
            for (entry = queue->PendingWrites.Flink;
                 entry != &queue->PendingWrites;
                 entry = entry->Flink) {
                PYUME_WRITE_REQUEST request;

                request = CONTAINING_RECORD(entry, YUME_WRITE_REQUEST, Link);
                DebugState->PendingWriteFragmentsIssued += request->NextIssueSeq;
                DebugState->PendingWriteFragmentsAcked += request->AckedCount;
            }
        }
        KeReleaseSpinLock(&queue->WriteQueueLock, oldIrql);
    }

    return STATUS_SUCCESS;
}
