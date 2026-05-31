#include "write.h"

#include "..\internal.h"
#include "..\slot\slot.h"
#include "..\..\core\memory.h"

static
ULONG
DiskBitmapWordCount(
    _In_ ULONG BitCount
)
{
    return (BitCount + (sizeof(ULONG) * 8u) - 1u) / (sizeof(ULONG) * 8u);
}

static
PYUME_WRITE_REQUEST
DiskAllocWriteRequest(
    _In_ PSTORAGE_REQUEST_BLOCK Srb,
    _In_ UINT64 EventId,
    _In_ UINT64 Lba,
    _In_ ULONG BlockCount,
    _In_ ULONG DataLength,
    _In_ ULONG SectorSize
)
{
    PYUME_WRITE_REQUEST request;
    ULONG maxSeq;
    ULONG bitmapWordCount;
    SIZE_T allocationSize;

    if (SectorSize == 0 || DataLength == 0 || (DataLength % SectorSize) != 0) {
        return NULL;
    }

    maxSeq = DataLength / SectorSize;
    bitmapWordCount = DiskBitmapWordCount(maxSeq);
    allocationSize =
        FIELD_OFFSET(YUME_WRITE_REQUEST, AckedBits) + ((SIZE_T)bitmapWordCount * sizeof(ULONG));

    request = (PYUME_WRITE_REQUEST)DiskAlloc(allocationSize);
    if (request == NULL) {
        return NULL;
    }

    RtlZeroMemory(request, allocationSize);
    request->Srb = Srb;
    request->EventId = EventId;
    request->BaseLba = Lba;
    request->BlockCount = BlockCount;
    request->TotalBytes = DataLength;
    request->MaxSeq = maxSeq;
    request->FinalStatus = STATUS_SUCCESS;
    RtlInitializeBitMap(&request->AckedBitmap, request->AckedBits, maxSeq);
    RtlClearAllBits(&request->AckedBitmap);
    return request;
}

static
NTSTATUS
DiskInitializeWriteRequestLocked(
    _Inout_ PYUME_WRITE_REQUEST Request,
    _In_ ULONG PayloadBytes
)
{
    ULONG totalSeq;

    if (Request->PayloadBytes != 0) {
        return STATUS_SUCCESS;
    }

    if (PayloadBytes == 0) {
        return STATUS_INVALID_PARAMETER;
    }

    totalSeq = (ULONG)((((UINT64)Request->TotalBytes) + PayloadBytes - 1u) / PayloadBytes);
    if (totalSeq == 0 || totalSeq > Request->MaxSeq) {
        return STATUS_INVALID_PARAMETER;
    }

    Request->PayloadBytes = PayloadBytes;
    Request->TotalSeq = totalSeq;
    return STATUS_SUCCESS;
}

static
PYUME_WRITE_REQUEST
DiskFindNextWritableRequestLocked(
    _Inout_ PYUME_DISK_QUEUE_STATE Queue
)
{
    PLIST_ENTRY entry;

    for (entry = Queue->PendingWrites.Flink;
         entry != &Queue->PendingWrites;
         entry = entry->Flink) {
        PYUME_WRITE_REQUEST request;

        request = CONTAINING_RECORD(entry, YUME_WRITE_REQUEST, Link);
        if (!NT_SUCCESS(DiskInitializeWriteRequestLocked(request, Queue->WriteSlotPayloadBytes))) {
            continue;
        }

        if (request->NextIssueSeq < request->TotalSeq) {
            return request;
        }
    }

    return NULL;
}

static
PYUME_WRITE_REQUEST
DiskFindWriteRequestByEventIdLocked(
    _In_ const PYUME_DISK_QUEUE_STATE Queue,
    _In_ UINT64 EventId
)
{
    PLIST_ENTRY entry;

    for (entry = Queue->PendingWrites.Flink;
         entry != &Queue->PendingWrites;
         entry = entry->Flink) {
        PYUME_WRITE_REQUEST request;

        request = CONTAINING_RECORD(entry, YUME_WRITE_REQUEST, Link);
        if (request->EventId == EventId) {
            return request;
        }
    }

    return NULL;
}

VOID
DiskCompleteWriteRequest(
    _In_ PVOID DeviceExtension,
    _In_ PYUME_WRITE_REQUEST Request
)
{
    if (NT_SUCCESS(Request->FinalStatus)) {
        InterlockedIncrement64(&((PDEVICE_CONTEXT)DeviceExtension)->DebugWriteRequestsCompleted);
        DiskCompleteScsiSrb(DeviceExtension, Request->Srb, STATUS_SUCCESS, Request->TotalBytes);
    } else {
        InterlockedIncrement64(&((PDEVICE_CONTEXT)DeviceExtension)->DebugWriteRequestsFailed);
        DiskCompleteScsiSrb(DeviceExtension, Request->Srb, Request->FinalStatus, 0);
    }

    DiskTickProgress((PDEVICE_CONTEXT)DeviceExtension);
    DiskFree(Request);
}

VOID
DiskDrainWriteSlots(
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
        PYUME_WRITE_REQUEST request;
        PYUME_POSTED_SLOT slot;
        ULONG seq;

        queue = &disk->Queue;
        KeAcquireSpinLock(&queue->WriteQueueLock, &oldIrql);
        request = DiskFindNextWritableRequestLocked(queue);
        if (request == NULL || IsListEmpty(&queue->PostedWriteSlots)) {
            KeReleaseSpinLock(&queue->WriteQueueLock, oldIrql);
            break;
        }

        slot = CONTAINING_RECORD(RemoveHeadList(&queue->PostedWriteSlots), YUME_POSTED_SLOT, Link);
        queue->PostedWriteSlotCount--;
        seq = request->NextIssueSeq++;
        KeReleaseSpinLock(&queue->WriteQueueLock, oldIrql);

        if (NT_SUCCESS(DiskWriteWriteSlotPayload(
                request->Srb->DataBuffer,
                request->EventId,
                request->BaseLba,
                request->TotalBytes,
                request->PayloadBytes,
                request->TotalSeq,
                seq,
                disk->SectorSize,
                slot))) {
            InterlockedIncrement64(&extension->DebugWriteFragmentsIssued);
            DiskTickProgress(extension);
            DiskCompleteSlotSrb(DeviceExtension, slot, STATUS_SUCCESS);
        } else {
            DiskCompleteSlotSrb(DeviceExtension, slot, STATUS_BUFFER_TOO_SMALL);
        }

        DiskFree(slot);
    }
}

NTSTATUS
DiskQueueWriteSrb(
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
    PYUME_WRITE_REQUEST request;
    KIRQL oldIrql;

    extension = (PDEVICE_CONTEXT)DeviceExtension;
    if (TargetId >= extension->MaxTargets || !DiskIsUsableTargetId(TargetId)) {
        return STATUS_INVALID_PARAMETER;
    }

    disk = &extension->Disk[TargetId];
    if (!disk->Present || !disk->Configured) {
        return STATUS_DEVICE_NOT_CONNECTED;
    }

    request = DiskAllocWriteRequest(
        Srb,
        DiskNextEventId(extension),
        Lba,
        BlockCount,
        DataLength,
        disk->SectorSize);
    if (request == NULL) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    KeAcquireSpinLock(&disk->Queue.WriteQueueLock, &oldIrql);
    InsertTailList(&disk->Queue.PendingWrites, &request->Link);
    disk->Queue.PendingWriteCount++;
    KeReleaseSpinLock(&disk->Queue.WriteQueueLock, oldIrql);

    InterlockedIncrement64(&extension->DebugWriteRequestsQueued);
    DiskTickProgress(extension);
    DiskDrainWriteSlots(DeviceExtension, TargetId);
    return STATUS_PENDING;
}

NTSTATUS
DiskHandleWriteAckBatchIoctl(
    _In_ PVOID DeviceExtension,
    _Inout_ PYUMEDISK_MESSAGE Message,
    _Inout_ PLIST_ENTRY DeferredWriteCompletions
)
{
    PDEVICE_CONTEXT extension;
    PYUME_DISK disk;
    PYUMEDISK_WRITE_ACK_BATCH batch;
    PYUMEDISK_WRITE_ACK_BATCH_RESULT result;
    PYUMEDISK_WRITE_ACK_FAILURE failures;
    ULONG index;
    ULONG failureCount;
    ULONG payloadCapacity;

    extension = (PDEVICE_CONTEXT)DeviceExtension;
    if (Message->Header.TargetId >= extension->MaxTargets ||
        !DiskIsUsableTargetId(Message->Header.TargetId) ||
        Message->Header.PayloadLength < (ULONG)YUMEDISK_WRITE_ACK_BATCH_BASE_SIZE) {
        return STATUS_INVALID_PARAMETER;
    }

    batch = (PYUMEDISK_WRITE_ACK_BATCH)Message->Payload;
    if (batch->RangeCount == 0 ||
        Message->Header.PayloadLength < (ULONG)YUMEDISK_WRITE_ACK_BATCH_SIZE(batch->RangeCount)) {
        return STATUS_INVALID_PARAMETER;
    }

    payloadCapacity = Message->Header.Size - YUMEDISK_MESSAGE_BASE_SIZE;
    disk = &extension->Disk[Message->Header.TargetId];
    result = (PYUMEDISK_WRITE_ACK_BATCH_RESULT)Message->Payload;
    failures = NULL;
    failureCount = 0;

    failures = (PYUMEDISK_WRITE_ACK_FAILURE)DiskAlloc(
        ((SIZE_T)batch->RangeCount) * sizeof(YUMEDISK_WRITE_ACK_FAILURE));
    if (failures == NULL) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    for (index = 0; index < batch->RangeCount; ++index) {
        PYUMEDISK_WRITE_ACK_RANGE range;
        PYUME_WRITE_REQUEST completedRequest;
        NTSTATUS rangeStatus;
        KIRQL oldIrql;
        UINT64 endSeq64;

        range = &batch->Ranges[index];
        completedRequest = NULL;
        rangeStatus = STATUS_SUCCESS;
        endSeq64 = (UINT64)range->SeqBase + range->SeqCount;

        if (range->EventId == 0 ||
            range->SeqCount == 0 ||
            endSeq64 > MAXULONG) {
            rangeStatus = STATUS_INVALID_PARAMETER;
        } else {
            KeAcquireSpinLock(&disk->Queue.WriteQueueLock, &oldIrql);
            {
                PYUME_WRITE_REQUEST request;
                ULONG seq;

                request = DiskFindWriteRequestByEventIdLocked(&disk->Queue, range->EventId);
                if (request == NULL) {
                    rangeStatus = STATUS_NOT_FOUND;
                } else if (!NT_SUCCESS(DiskInitializeWriteRequestLocked(request, disk->Queue.WriteSlotPayloadBytes))) {
                    rangeStatus = STATUS_INVALID_PARAMETER;
                } else if (endSeq64 > request->TotalSeq || endSeq64 > request->NextIssueSeq) {
                    rangeStatus = STATUS_INVALID_PARAMETER;
                } else {
                    for (seq = range->SeqBase; seq < (ULONG)endSeq64; ++seq) {
                        if (RtlCheckBit(&request->AckedBitmap, seq)) {
                            rangeStatus = STATUS_INVALID_PARAMETER;
                            break;
                        }
                    }

                    if (NT_SUCCESS(rangeStatus)) {
                        for (seq = range->SeqBase; seq < (ULONG)endSeq64; ++seq) {
                            RtlSetBits(&request->AckedBitmap, seq, 1);
                        }

                        request->AckedCount += range->SeqCount;
                        InterlockedAdd64(&extension->DebugWriteAcksApplied, range->SeqCount);

                        if (!NT_SUCCESS(range->IoStatus)) {
                            request->FinalStatus = range->IoStatus;
                            RemoveEntryList(&request->Link);
                            disk->Queue.PendingWriteCount--;
                            DiskResetWriteSlotShapeLocked(&disk->Queue);
                            completedRequest = request;
                        } else if (request->AckedCount == request->TotalSeq) {
                            RemoveEntryList(&request->Link);
                            disk->Queue.PendingWriteCount--;
                            DiskResetWriteSlotShapeLocked(&disk->Queue);
                            completedRequest = request;
                        }
                    }
                }
            }
            KeReleaseSpinLock(&disk->Queue.WriteQueueLock, oldIrql);
        }

        if (NT_SUCCESS(rangeStatus) && completedRequest != NULL) {
            InsertTailList(DeferredWriteCompletions, &completedRequest->Link);
            DiskTickProgress(extension);
        } else if (!NT_SUCCESS(rangeStatus)) {
            if (YUMEDISK_WRITE_ACK_BATCH_RESULT_SIZE(failureCount + 1) > payloadCapacity) {
                DiskFree(failures);
                return STATUS_BUFFER_TOO_SMALL;
            }

            failures[failureCount].RangeIndex = index;
            failures[failureCount].Status = rangeStatus;
            failureCount++;
        }
    }

    DiskInitMessageStatus(
        Message,
        YumeDiskCommandWriteAckBatch,
        STATUS_SUCCESS,
        (failureCount == 0) ? 0 : YUMEDISK_WRITE_ACK_BATCH_RESULT_SIZE(failureCount));
    if (failureCount != 0) {
        result->FailureCount = failureCount;
        result->Reserved = 0;
        RtlCopyMemory(
            result->Failures,
            failures,
            ((SIZE_T)failureCount) * sizeof(YUMEDISK_WRITE_ACK_FAILURE));
    }

    DiskFree(failures);
    return STATUS_SUCCESS;
}

VOID
DiskCompleteDeferredWriteCompletions(
    _In_ PVOID DeviceExtension,
    _Inout_ PLIST_ENTRY DeferredWriteCompletions
)
{
    while (!IsListEmpty(DeferredWriteCompletions)) {
        PYUME_WRITE_REQUEST request;

        request = CONTAINING_RECORD(RemoveHeadList(DeferredWriteCompletions), YUME_WRITE_REQUEST, Link);
        DiskCompleteWriteRequest(DeviceExtension, request);
    }
}
