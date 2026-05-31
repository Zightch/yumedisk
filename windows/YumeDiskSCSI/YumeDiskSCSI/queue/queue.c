#include "queue.h"
#include "internal.h"
#include "read/read.h"
#include "slot/slot.h"

#include "..\core\memory.h"

typedef struct _YUME_WRITE_REQUEST {
    LIST_ENTRY Link;
    PSTORAGE_REQUEST_BLOCK Srb;
    UINT64 EventId;
    UINT64 BaseLba;
    ULONG BlockCount;
    ULONG TotalBytes;
    ULONG MaxSeq;
    ULONG TotalSeq;
    ULONG PayloadBytes;
    ULONG NextIssueSeq;
    ULONG AckedCount;
    NTSTATUS FinalStatus;
    RTL_BITMAP AckedBitmap;
    ULONG AckedBits[1];
} YUME_WRITE_REQUEST, *PYUME_WRITE_REQUEST;

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
VOID
DiskCompletePendingEventSlot(
    _In_ PVOID DeviceExtension,
    _In_ ULONG TargetId,
    _In_ NTSTATUS Status,
    _In_opt_ const YUMEDISK_DISK_EVENT* EventRecord
)
{
    PDEVICE_CONTEXT extension;
    PYUME_DISK disk;
    PSTORAGE_REQUEST_BLOCK pendingSrb;
    UINT64 kernelVa;
    ULONG capacity;
    KIRQL oldIrql;

    extension = (PDEVICE_CONTEXT)DeviceExtension;
    if (TargetId >= extension->MaxTargets) {
        return;
    }

    disk = &extension->Disk[TargetId];
    pendingSrb = NULL;
    kernelVa = 0ull;
    capacity = 0u;

    KeAcquireSpinLock(&disk->EventSlot.Lock, &oldIrql);
    pendingSrb = disk->EventSlot.PendingSrb;
    if (pendingSrb != NULL) {
        kernelVa = disk->EventSlot.KernelVa;
        capacity = disk->EventSlot.Capacity;
        disk->EventSlot.PendingSrb = NULL;
        disk->EventSlot.SlotId = 0ull;
        disk->EventSlot.KernelVa = 0ull;
        disk->EventSlot.Capacity = 0u;
        disk->EventSlot.Flags = 0u;
    }
    KeReleaseSpinLock(&disk->EventSlot.Lock, oldIrql);

    if (pendingSrb == NULL) {
        return;
    }

    DiskCompleteEventSlotSrb(
        DeviceExtension,
        (UCHAR)TargetId,
        pendingSrb,
        kernelVa,
        capacity,
        Status,
        EventRecord);
}

static
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

static
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
        InitializeListHead(&Extension->Disk[index].Queue.PostedReadSlots);
        InitializeListHead(&Extension->Disk[index].Queue.PendingReads);
        InitializeListHead(&Extension->Disk[index].Queue.PostedWriteSlots);
        InitializeListHead(&Extension->Disk[index].Queue.PendingWrites);
        Extension->Disk[index].Queue.PostedReadSlotCount = 0;
        Extension->Disk[index].Queue.PendingReadCount = 0;
        Extension->Disk[index].Queue.PendingReadIssuedCount = 0;
        Extension->Disk[index].Queue.PostedWriteSlotCount = 0;
        Extension->Disk[index].Queue.PendingWriteCount = 0;
        Extension->Disk[index].Queue.WriteSlotPayloadBytes = 0;
        Extension->Disk[index].EventSlot.PendingSrb = NULL;
        Extension->Disk[index].EventSlot.SlotId = 0ull;
        Extension->Disk[index].EventSlot.KernelVa = 0ull;
        Extension->Disk[index].EventSlot.Capacity = 0u;
        Extension->Disk[index].EventSlot.Flags = 0u;
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
        InitializeListHead(&extension->Disk[index].Queue.PostedReadSlots);
        InitializeListHead(&extension->Disk[index].Queue.PendingReads);
        InitializeListHead(&extension->Disk[index].Queue.PostedWriteSlots);
        InitializeListHead(&extension->Disk[index].Queue.PendingWrites);
        extension->Disk[index].Queue.PostedReadSlotCount = 0;
        extension->Disk[index].Queue.PendingReadCount = 0;
        extension->Disk[index].Queue.PendingReadIssuedCount = 0;
        extension->Disk[index].Queue.PostedWriteSlotCount = 0;
        extension->Disk[index].Queue.PendingWriteCount = 0;
        extension->Disk[index].Queue.WriteSlotPayloadBytes = 0;
        extension->Disk[index].EventSlot.PendingSrb = NULL;
        extension->Disk[index].EventSlot.SlotId = 0ull;
        extension->Disk[index].EventSlot.KernelVa = 0ull;
        extension->Disk[index].EventSlot.Capacity = 0u;
        extension->Disk[index].EventSlot.Flags = 0u;
    }
}

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

    request = DiskAllocWriteRequest(Srb, DiskNextEventId(extension), Lba, BlockCount, DataLength, disk->SectorSize);
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
DiskHandleSubmitEventSlotIoctl(
    _In_ PVOID DeviceExtension,
    _In_ PSTORAGE_REQUEST_BLOCK Srb,
    _Inout_ PYUMEDISK_MESSAGE Message
)
{
    PDEVICE_CONTEXT extension;
    PYUMEDISK_SUBMIT_EVENT_SLOT submitEventSlot;
    YUMEDISK_EVENT_SLOT_DESCRIPTOR* slotDescriptor;
    PYUME_DISK disk;
    KIRQL oldIrql;
    NTSTATUS status;

    extension = (PDEVICE_CONTEXT)DeviceExtension;
    if (Message->Header.PayloadLength < sizeof(*submitEventSlot)) {
        return STATUS_INVALID_PARAMETER;
    }

    submitEventSlot = (PYUMEDISK_SUBMIT_EVENT_SLOT)Message->Payload;
    slotDescriptor = &submitEventSlot->Slot;
    if (slotDescriptor->SessionId != Message->Header.SessionId ||
        slotDescriptor->TargetId != Message->Header.TargetId ||
        slotDescriptor->TargetId >= extension->MaxTargets ||
        !DiskIsUsableTargetId(slotDescriptor->TargetId) ||
        slotDescriptor->SlotId == 0ull ||
        slotDescriptor->KernelVa == 0ull ||
        slotDescriptor->Capacity < sizeof(YUMEDISK_DISK_EVENT)) {
        return STATUS_INVALID_PARAMETER;
    }

    disk = &extension->Disk[slotDescriptor->TargetId];
    if (!disk->Present || !disk->Configured) {
        return STATUS_DEVICE_NOT_CONNECTED;
    }

    status = STATUS_PENDING;
    KeAcquireSpinLock(&disk->EventSlot.Lock, &oldIrql);
    if (disk->EventSlot.PendingSrb != NULL) {
        status = STATUS_DEVICE_BUSY;
    } else {
        disk->EventSlot.PendingSrb = Srb;
        disk->EventSlot.SlotId = slotDescriptor->SlotId;
        disk->EventSlot.KernelVa = slotDescriptor->KernelVa;
        disk->EventSlot.Capacity = slotDescriptor->Capacity;
        disk->EventSlot.Flags = slotDescriptor->Flags;
    }
    KeReleaseSpinLock(&disk->EventSlot.Lock, oldIrql);

    return status;
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
        RtlCopyMemory(result->Failures, failures, ((SIZE_T)failureCount) * sizeof(YUMEDISK_WRITE_ACK_FAILURE));
    }

    DiskFree(failures);
    return STATUS_SUCCESS;
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
