#include "queue.h"

#include "..\core\memory.h"

typedef struct _YUME_POSTED_SLOT {
    LIST_ENTRY Link;
    PSTORAGE_REQUEST_BLOCK Srb;
    UINT64 SlotId;
    UINT64 KernelVa;
    ULONG Capacity;
    ULONG TargetId;
    ULONG SlotType;
} YUME_POSTED_SLOT, *PYUME_POSTED_SLOT;

typedef struct _YUME_READ_REQUEST {
    LIST_ENTRY Link;
    PSTORAGE_REQUEST_BLOCK Srb;
    UINT64 EventId;
    UINT64 Lba;
    ULONG BlockCount;
    ULONG DataLength;
    BOOLEAN SlotIssued;
} YUME_READ_REQUEST, *PYUME_READ_REQUEST;

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
VOID
DiskResetWriteSlotShapeLocked(
    _Inout_ PYUME_DISK_QUEUE_STATE Queue
)
{
    if (Queue->PostedWriteSlotCount == 0 && Queue->PendingWriteCount == 0) {
        Queue->WriteSlotPayloadBytes = 0;
    }
}

static
ULONG
DiskBitmapWordCount(
    _In_ ULONG BitCount
)
{
    return (BitCount + (sizeof(ULONG) * 8u) - 1u) / (sizeof(ULONG) * 8u);
}

static
UINT64
DiskNextEventId(
    _Inout_ PDEVICE_CONTEXT Extension
)
{
    return (UINT64)InterlockedIncrement64(&Extension->NextEventId);
}

static
VOID
DiskTickProgress(
    _Inout_ PDEVICE_CONTEXT Extension
)
{
    InterlockedIncrement64(&Extension->DebugProgressCounter);
}

static
PYUME_POSTED_SLOT
DiskAllocPostedSlot(
    _In_ PSTORAGE_REQUEST_BLOCK Srb,
    _In_ const YUMEDISK_SLOT_DESCRIPTOR* Slot
)
{
    PYUME_POSTED_SLOT postedSlot;

    postedSlot = (PYUME_POSTED_SLOT)DiskAlloc(sizeof(*postedSlot));
    if (postedSlot == NULL) {
        return NULL;
    }

    RtlZeroMemory(postedSlot, sizeof(*postedSlot));
    postedSlot->Srb = Srb;
    postedSlot->SlotId = Slot->SlotId;
    postedSlot->KernelVa = Slot->KernelVa;
    postedSlot->Capacity = Slot->Capacity;
    postedSlot->TargetId = Slot->TargetId;
    postedSlot->SlotType = Slot->SlotType;
    return postedSlot;
}

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
ULONG
DiskComputeWritePayloadBytes(
    _In_ ULONG SectorSize,
    _In_ ULONG SlotCapacity
)
{
    ULONG payloadBytes;

    if (SectorSize == 0 || SlotCapacity <= (ULONG)YUMEDISK_WRITE_SLOT_HEADER_BASE_SIZE) {
        return 0;
    }

    payloadBytes = SlotCapacity - YUMEDISK_WRITE_SLOT_HEADER_BASE_SIZE;
    payloadBytes -= (payloadBytes % SectorSize);
    return payloadBytes;
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
PSRB_IO_CONTROL
DiskGetIoctlControl(
    _In_ PSTORAGE_REQUEST_BLOCK Srb
)
{
    return (PSRB_IO_CONTROL)Srb->DataBuffer;
}

static
PYUMEDISK_MESSAGE
DiskGetIoctlMessage(
    _In_ PSTORAGE_REQUEST_BLOCK Srb
)
{
    return (PYUMEDISK_MESSAGE)(DiskGetIoctlControl(Srb) + 1);
}

static
VOID
DiskCompleteSlotSrb(
    _In_ PVOID DeviceExtension,
    _In_ PYUME_POSTED_SLOT Slot,
    _In_ NTSTATUS Status
)
{
    PSRB_IO_CONTROL srbIoControl;
    PYUMEDISK_MESSAGE message;

    srbIoControl = DiskGetIoctlControl(Slot->Srb);
    message = DiskGetIoctlMessage(Slot->Srb);
    DiskInitMessageStatus(message, YumeDiskCommandSubmitSlot, Status, 0);
    DiskCompleteIoctlSrb(Slot->Srb, srbIoControl, Status, message->Header.Size);
    StorPortNotification(RequestComplete, DeviceExtension, Slot->Srb);
}

static
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
NTSTATUS
DiskWriteReadSlotEvent(
    _In_ const PYUME_READ_REQUEST Request,
    _In_ const PYUME_POSTED_SLOT Slot
)
{
    PYUMEDISK_READ_SLOT_EVENT readEvent;

    if (Slot->KernelVa == 0 || Slot->Capacity < sizeof(*readEvent)) {
        return STATUS_BUFFER_TOO_SMALL;
    }

    readEvent = (PYUMEDISK_READ_SLOT_EVENT)(ULONG_PTR)Slot->KernelVa;
    readEvent->EventId = Request->EventId;
    readEvent->TargetId = Slot->TargetId;
    readEvent->Reserved0 = 0;
    readEvent->Lba = Request->Lba;
    readEvent->BlockCount = Request->BlockCount;
    readEvent->DataLength = Request->DataLength;
    return STATUS_SUCCESS;
}

static
NTSTATUS
DiskWriteWriteSlotPayload(
    _In_ const PYUME_DISK Disk,
    _In_ const PYUME_WRITE_REQUEST Request,
    _In_ ULONG Seq,
    _In_ const PYUME_POSTED_SLOT Slot
)
{
    PYUMEDISK_WRITE_SLOT_HEADER header;
    ULONG byteOffset;
    ULONG remainingBytes;
    ULONG fragmentBytes;

    if (Slot->KernelVa == 0 || Request->PayloadBytes == 0 || Request->TotalSeq == 0) {
        return STATUS_INVALID_PARAMETER;
    }

    byteOffset = Seq * Request->PayloadBytes;
    remainingBytes = Request->TotalBytes - byteOffset;
    fragmentBytes = (remainingBytes < Request->PayloadBytes) ? remainingBytes : Request->PayloadBytes;
    if (Slot->Capacity < YUMEDISK_WRITE_SLOT_HEADER_BASE_SIZE + fragmentBytes) {
        return STATUS_BUFFER_TOO_SMALL;
    }

    header = (PYUMEDISK_WRITE_SLOT_HEADER)(ULONG_PTR)Slot->KernelVa;
    header->EventId = Request->EventId;
    header->Seq = Seq;
    header->TotalSeq = Request->TotalSeq;
    header->TargetId = Slot->TargetId;
    header->Reserved0 = 0;
    header->Lba = Request->BaseLba + (byteOffset / Disk->SectorSize);
    header->ByteOffsetInWrite = byteOffset;
    header->DataLength = fragmentBytes;
    header->Flags = 0;
    header->Reserved1 = 0;
    RtlCopyMemory(
        header->Data,
        (PUCHAR)Request->Srb->DataBuffer + byteOffset,
        fragmentBytes);
    return STATUS_SUCCESS;
}

static
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

        if (NT_SUCCESS(DiskWriteReadSlotEvent(request, slot))) {
            InterlockedIncrement64(&extension->DebugReadSlotsIssued);
            DiskTickProgress(extension);
            DiskCompleteSlotSrb(DeviceExtension, slot, STATUS_SUCCESS);
        } else {
            DiskCompleteSlotSrb(DeviceExtension, slot, STATUS_BUFFER_TOO_SMALL);
        }

        DiskFree(slot);
    }
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

        if (NT_SUCCESS(DiskWriteWriteSlotPayload(disk, request, seq, slot))) {
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
    }
}

VOID
DiskCompleteTargetPending(
    _In_ PVOID DeviceExtension,
    _In_ ULONG TargetId,
    _In_ NTSTATUS Status
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
        DiskCompleteReadRequest(DeviceExtension, request, Status);
    }

    while (!IsListEmpty(&readSlots)) {
        PYUME_POSTED_SLOT slot;

        slot = CONTAINING_RECORD(RemoveHeadList(&readSlots), YUME_POSTED_SLOT, Link);
        DiskCompleteSlotSrb(DeviceExtension, slot, Status);
        DiskFree(slot);
    }

    while (!IsListEmpty(&writeRequests)) {
        PYUME_WRITE_REQUEST request;

        request = CONTAINING_RECORD(RemoveHeadList(&writeRequests), YUME_WRITE_REQUEST, Link);
        request->FinalStatus = Status;
        DiskCompleteWriteRequest(DeviceExtension, request);
    }

    while (!IsListEmpty(&writeSlots)) {
        PYUME_POSTED_SLOT slot;

        slot = CONTAINING_RECORD(RemoveHeadList(&writeSlots), YUME_POSTED_SLOT, Link);
        DiskCompleteSlotSrb(DeviceExtension, slot, Status);
        DiskFree(slot);
    }
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
    PLIST_ENTRY entry;
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
        for (entry = disk->Queue.PostedReadSlots.Flink;
             entry != &disk->Queue.PostedReadSlots;
             entry = entry->Flink) {
            PYUME_POSTED_SLOT slot;

            slot = CONTAINING_RECORD(entry, YUME_POSTED_SLOT, Link);
            if (slot->SlotId == cancelSlot->SlotId) {
                RemoveEntryList(&slot->Link);
                disk->Queue.PostedReadSlotCount--;
                removedSlot = slot;
                break;
            }
        }
        KeReleaseSpinLock(&disk->Queue.ReadQueueLock, oldIrql);
    } else if (cancelSlot->SlotType == YumeDiskSlotTypeWrite) {
        KeAcquireSpinLock(&disk->Queue.WriteQueueLock, &oldIrql);
        for (entry = disk->Queue.PostedWriteSlots.Flink;
             entry != &disk->Queue.PostedWriteSlots;
             entry = entry->Flink) {
            PYUME_POSTED_SLOT slot;

            slot = CONTAINING_RECORD(entry, YUME_POSTED_SLOT, Link);
            if (slot->SlotId == cancelSlot->SlotId) {
                RemoveEntryList(&slot->Link);
                disk->Queue.PostedWriteSlotCount--;
                DiskResetWriteSlotShapeLocked(&disk->Queue);
                removedSlot = slot;
                break;
            }
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
