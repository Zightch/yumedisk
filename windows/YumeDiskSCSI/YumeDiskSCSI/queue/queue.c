#include "queue.h"

#include "..\core\memory.h"

typedef struct _DISK_POSTED_READ_SLOT {
    LIST_ENTRY Link;
    PSTORAGE_REQUEST_BLOCK ControlSrb;
    UINT64 SlotId;
    ULONG TargetId;
    PUCHAR BufferVa;
    ULONG Capacity;
} DISK_POSTED_READ_SLOT, *PDISK_POSTED_READ_SLOT;

typedef struct _DISK_PENDING_READ {
    LIST_ENTRY Link;
    PSTORAGE_REQUEST_BLOCK Srb;
    UINT64 EventId;
    UINT64 IssuedSlotId;
    ULONG TargetId;
    UINT64 Lba;
    ULONG BlockCount;
    ULONG DataLength;
    BOOLEAN SlotIssued;
} DISK_PENDING_READ, *PDISK_PENDING_READ;

typedef struct _DISK_POSTED_WRITE_SLOT {
    LIST_ENTRY Link;
    PSTORAGE_REQUEST_BLOCK ControlSrb;
    UINT64 SlotId;
    ULONG TargetId;
    PUCHAR BufferVa;
    ULONG Capacity;
} DISK_POSTED_WRITE_SLOT, *PDISK_POSTED_WRITE_SLOT;

typedef struct _DISK_PENDING_WRITE {
    LIST_ENTRY Link;
    PSTORAGE_REQUEST_BLOCK Srb;
    UINT64 EventId;
    UINT64 LastIssuedSlotId;
    ULONG TargetId;
    UINT64 BaseLba;
    ULONG TotalBytes;
    ULONG SectorSize;
    ULONG FragmentBytes;
    ULONG TotalSeq;
    ULONG NextIssueSeq;
    ULONG AckedCount;
    NTSTATUS CompletionStatus;
    ULONG CompletionTransferLength;
    RTL_BITMAP AckedBitmap;
    ULONG AckedBits[1];
} DISK_PENDING_WRITE, *PDISK_PENDING_WRITE;

static
PYUME_DISK
DiskGetTargetDisk(
    _Inout_ PDEVICE_CONTEXT Extension,
    _In_ ULONG TargetId
)
{
    if (TargetId >= Extension->MaxTargets || !DiskIsUsableTargetId(TargetId)) {
        return NULL;
    }

    return &Extension->Disk[TargetId];
}

static
UINT64
DiskAllocateEventId(
    _Inout_ PDEVICE_CONTEXT Extension
)
{
    LONG64 value;

    do {
        value = InterlockedIncrement64(&Extension->NextEventId);
    } while (value == 0);

    return (UINT64)value;
}

static
VOID
DiskDebugMarkProgress(
    _Inout_ PDEVICE_CONTEXT Extension
)
{
    InterlockedIncrement64(&Extension->DebugProgressCounter);
}

static
VOID
DiskDebugIncrement(
    _Inout_ PDEVICE_CONTEXT Extension,
    _Inout_ volatile LONG64* Counter
)
{
    InterlockedIncrement64(Counter);
    DiskDebugMarkProgress(Extension);
}

static
VOID
DiskDebugAdd(
    _Inout_ PDEVICE_CONTEXT Extension,
    _Inout_ volatile LONG64* Counter,
    _In_ LONG64 Delta
)
{
    InterlockedAdd64(Counter, Delta);
    DiskDebugMarkProgress(Extension);
}

static
BOOLEAN
DiskGetIoctlResponseBuffers(
    _In_ PSTORAGE_REQUEST_BLOCK Srb,
    _Out_ PSRB_IO_CONTROL* SrbIoControl,
    _Out_ PYUMEDISK_MESSAGE* Message
)
{
    PSRB_IO_CONTROL srbIoControl;

    if (Srb == NULL || Srb->DataBuffer == NULL) {
        return FALSE;
    }

    srbIoControl = (PSRB_IO_CONTROL)Srb->DataBuffer;
    if (srbIoControl->HeaderLength != sizeof(SRB_IO_CONTROL)) {
        return FALSE;
    }

    *SrbIoControl = srbIoControl;
    *Message = (PYUMEDISK_MESSAGE)(srbIoControl + 1);
    return TRUE;
}

static
VOID
DiskCompleteSubmitSlotSrb(
    _In_ PVOID DeviceExtension,
    _In_ PSTORAGE_REQUEST_BLOCK Srb,
    _In_ NTSTATUS Status
)
{
    PSRB_IO_CONTROL srbIoControl;
    PYUMEDISK_MESSAGE message;

    if (!DiskGetIoctlResponseBuffers(Srb, &srbIoControl, &message)) {
        return;
    }

    DiskInitMessageStatus(message, YumeDiskCommandSubmitSlot, Status, 0);
    DiskCompleteIoctlSrb(Srb, srbIoControl, Status, message->Header.Size);
    StorPortNotification(RequestComplete, DeviceExtension, Srb);
}

static
VOID
DiskCompleteDetachedReadSlots(
    _In_ PVOID DeviceExtension,
    _Inout_ PLIST_ENTRY CompletedSlots,
    _In_ NTSTATUS Status
)
{
    while (!IsListEmpty(CompletedSlots)) {
        PDISK_POSTED_READ_SLOT slot;

        slot = CONTAINING_RECORD(RemoveHeadList(CompletedSlots), DISK_POSTED_READ_SLOT, Link);
        DiskCompleteSubmitSlotSrb(DeviceExtension, slot->ControlSrb, Status);
        DiskFree(slot);
    }
}

static
VOID
DiskCompleteDetachedWriteSlots(
    _In_ PVOID DeviceExtension,
    _Inout_ PLIST_ENTRY CompletedSlots,
    _In_ NTSTATUS Status
)
{
    while (!IsListEmpty(CompletedSlots)) {
        PDISK_POSTED_WRITE_SLOT slot;

        slot = CONTAINING_RECORD(RemoveHeadList(CompletedSlots), DISK_POSTED_WRITE_SLOT, Link);
        DiskCompleteSubmitSlotSrb(DeviceExtension, slot->ControlSrb, Status);
        DiskFree(slot);
    }
}

static
VOID
DiskCompleteDetachedReads(
    _In_ PVOID DeviceExtension,
    _Inout_ PLIST_ENTRY CompletedReads,
    _In_ NTSTATUS Status
)
{
    PDEVICE_CONTEXT extension;

    extension = (PDEVICE_CONTEXT)DeviceExtension;
    while (!IsListEmpty(CompletedReads)) {
        PDISK_PENDING_READ request;

        request = CONTAINING_RECORD(RemoveHeadList(CompletedReads), DISK_PENDING_READ, Link);
        if (NT_SUCCESS(Status)) {
            DiskDebugIncrement(extension, &extension->DebugReadRequestsCompleted);
        } else {
            DiskDebugIncrement(extension, &extension->DebugReadRequestsFailed);
        }
        DiskCompleteScsiSrb(DeviceExtension, request->Srb, Status, 0);
        DiskFree(request);
    }
}

static
VOID
DiskCompleteDetachedWrites(
    _In_ PVOID DeviceExtension,
    _Inout_ PLIST_ENTRY CompletedWrites,
    _In_ NTSTATUS Status
)
{
    PDEVICE_CONTEXT extension;

    extension = (PDEVICE_CONTEXT)DeviceExtension;
    while (!IsListEmpty(CompletedWrites)) {
        PDISK_PENDING_WRITE request;

        request = CONTAINING_RECORD(RemoveHeadList(CompletedWrites), DISK_PENDING_WRITE, Link);
        if (NT_SUCCESS(Status)) {
            DiskDebugIncrement(extension, &extension->DebugWriteRequestsCompleted);
        } else {
            DiskDebugIncrement(extension, &extension->DebugWriteRequestsFailed);
        }
        DiskCompleteScsiSrb(DeviceExtension, request->Srb, Status, 0);
        DiskFree(request);
    }
}

static
VOID
DiskCompleteAckedWrites(
    _In_ PVOID DeviceExtension,
    _Inout_ PLIST_ENTRY CompletedWrites
)
{
    PDEVICE_CONTEXT extension;

    extension = (PDEVICE_CONTEXT)DeviceExtension;
    while (!IsListEmpty(CompletedWrites)) {
        PDISK_PENDING_WRITE request;

        request = CONTAINING_RECORD(RemoveHeadList(CompletedWrites), DISK_PENDING_WRITE, Link);
        if (NT_SUCCESS(request->CompletionStatus)) {
            DiskDebugIncrement(extension, &extension->DebugWriteRequestsCompleted);
        } else {
            DiskDebugIncrement(extension, &extension->DebugWriteRequestsFailed);
        }
        DiskCompleteScsiSrb(
            DeviceExtension,
            request->Srb,
            request->CompletionStatus,
            request->CompletionTransferLength);
        DiskFree(request);
    }
}

static
PDISK_PENDING_READ
DiskLookupReadRequestLocked(
    _In_ PYUME_DISK Disk,
    _In_ UINT64 EventId
)
{
    PLIST_ENTRY entry;

    for (entry = Disk->Queue.PendingReads.Flink;
        entry != &Disk->Queue.PendingReads;
        entry = entry->Flink) {
        PDISK_PENDING_READ request;

        request = CONTAINING_RECORD(entry, DISK_PENDING_READ, Link);
        if (request->EventId == EventId) {
            return request;
        }
    }

    return NULL;
}

static
PDISK_PENDING_WRITE
DiskLookupWriteRequestLocked(
    _In_ PYUME_DISK Disk,
    _In_ UINT64 EventId
)
{
    PLIST_ENTRY entry;

    for (entry = Disk->Queue.PendingWrites.Flink;
        entry != &Disk->Queue.PendingWrites;
        entry = entry->Flink) {
        PDISK_PENDING_WRITE request;

        request = CONTAINING_RECORD(entry, DISK_PENDING_WRITE, Link);
        if (request->EventId == EventId) {
            return request;
        }
    }

    return NULL;
}

static
ULONG
DiskAlignWritePayloadBytes(
    _In_ ULONG Capacity,
    _In_ ULONG SectorSize
)
{
    ULONG payloadBytes;

    if (Capacity <= (ULONG)YUMEDISK_WRITE_SLOT_HEADER_BASE_SIZE || SectorSize == 0) {
        return 0;
    }

    payloadBytes = Capacity - (ULONG)YUMEDISK_WRITE_SLOT_HEADER_BASE_SIZE;
    return (payloadBytes / SectorSize) * SectorSize;
}

static
BOOLEAN
DiskWriteRequestCanIssueToSlotLocked(
    _In_ const DISK_PENDING_WRITE* Request,
    _In_ ULONG SlotPayloadBytes
)
{
    ULONGLONG byteOffset;
    ULONG remainingBytes;
    ULONG requiredBytes;

    if (Request->NextIssueSeq >= Request->TotalSeq && Request->FragmentBytes != 0) {
        return FALSE;
    }

    if (Request->FragmentBytes == 0) {
        return SlotPayloadBytes >= Request->SectorSize;
    }

    byteOffset = (ULONGLONG)Request->NextIssueSeq * Request->FragmentBytes;
    if (byteOffset >= Request->TotalBytes) {
        return FALSE;
    }

    remainingBytes = Request->TotalBytes - (ULONG)byteOffset;
    requiredBytes = min(Request->FragmentBytes, remainingBytes);
    return SlotPayloadBytes >= requiredBytes;
}

static
BOOLEAN
DiskPopReadDispatchLocked(
    _Inout_ PYUME_DISK Disk,
    _Out_ PDISK_POSTED_READ_SLOT* Slot,
    _Out_ PDISK_PENDING_READ* Request
)
{
    PLIST_ENTRY requestEntry;

    for (requestEntry = Disk->Queue.PendingReads.Flink;
        requestEntry != &Disk->Queue.PendingReads;
        requestEntry = requestEntry->Flink) {
        PDISK_PENDING_READ candidateRequest;
        PLIST_ENTRY slotEntry;

        candidateRequest = CONTAINING_RECORD(requestEntry, DISK_PENDING_READ, Link);
        if (candidateRequest->SlotIssued) {
            continue;
        }

        slotEntry = Disk->Queue.PostedReadSlots.Flink;
        if (slotEntry != &Disk->Queue.PostedReadSlots) {
            PDISK_POSTED_READ_SLOT candidateSlot;

            candidateSlot = CONTAINING_RECORD(slotEntry, DISK_POSTED_READ_SLOT, Link);
            RemoveEntryList(&candidateSlot->Link);
            candidateRequest->SlotIssued = TRUE;
            candidateRequest->IssuedSlotId = candidateSlot->SlotId;
            *Slot = candidateSlot;
            *Request = candidateRequest;
            return TRUE;
        }
    }

    *Slot = NULL;
    *Request = NULL;
    return FALSE;
}

static
BOOLEAN
DiskPopWriteDispatchLocked(
    _Inout_ PYUME_DISK Disk,
    _Out_ PDISK_POSTED_WRITE_SLOT* Slot,
    _Out_ PDISK_PENDING_WRITE* Request,
    _Out_ PULONG Seq,
    _Out_ PULONG ByteOffset,
    _Out_ PULONG DataLength
)
{
    PLIST_ENTRY requestEntry;

    for (requestEntry = Disk->Queue.PendingWrites.Flink;
        requestEntry != &Disk->Queue.PendingWrites;
        requestEntry = requestEntry->Flink) {
        PDISK_PENDING_WRITE candidateRequest;
        PLIST_ENTRY slotEntry;

        candidateRequest = CONTAINING_RECORD(requestEntry, DISK_PENDING_WRITE, Link);
        for (slotEntry = Disk->Queue.PostedWriteSlots.Flink;
            slotEntry != &Disk->Queue.PostedWriteSlots;
            slotEntry = slotEntry->Flink) {
            PDISK_POSTED_WRITE_SLOT candidateSlot;
            ULONG slotPayloadBytes;
            ULONGLONG byteOffset64;
            ULONG remainingBytes;
            ULONG fragmentBytes;

            candidateSlot = CONTAINING_RECORD(slotEntry, DISK_POSTED_WRITE_SLOT, Link);
            slotPayloadBytes = DiskAlignWritePayloadBytes(candidateSlot->Capacity, candidateRequest->SectorSize);
            if (!DiskWriteRequestCanIssueToSlotLocked(candidateRequest, slotPayloadBytes)) {
                continue;
            }

            if (candidateRequest->FragmentBytes == 0) {
                candidateRequest->FragmentBytes = min(slotPayloadBytes, candidateRequest->TotalBytes);
                candidateRequest->TotalSeq = (ULONG)(
                    ((ULONGLONG)candidateRequest->TotalBytes + candidateRequest->FragmentBytes - 1) /
                    candidateRequest->FragmentBytes);
            }

            *Seq = candidateRequest->NextIssueSeq;
            byteOffset64 = (ULONGLONG)(*Seq) * candidateRequest->FragmentBytes;
            remainingBytes = candidateRequest->TotalBytes - (ULONG)byteOffset64;
            fragmentBytes = min(candidateRequest->FragmentBytes, remainingBytes);

            candidateRequest->NextIssueSeq++;
            candidateRequest->LastIssuedSlotId = candidateSlot->SlotId;
            RemoveEntryList(&candidateSlot->Link);

            *Slot = candidateSlot;
            *Request = candidateRequest;
            *ByteOffset = (ULONG)byteOffset64;
            *DataLength = fragmentBytes;
            return TRUE;
        }
    }

    *Slot = NULL;
    *Request = NULL;
    *Seq = 0;
    *ByteOffset = 0;
    *DataLength = 0;
    return FALSE;
}

static
VOID
DiskDrainReadSlots(
    _In_ PVOID DeviceExtension,
    _Inout_ PYUME_DISK Disk,
    _In_opt_ PSTORAGE_REQUEST_BLOCK CurrentControlSrb,
    _Out_opt_ BOOLEAN* CurrentCompleted
)
{
    PDEVICE_CONTEXT extension;

    extension = (PDEVICE_CONTEXT)DeviceExtension;
    for (;;) {
        PDISK_POSTED_READ_SLOT slot;
        PDISK_PENDING_READ request;
        KIRQL oldIrql;
        PYUMEDISK_READ_SLOT_EVENT event;

        KeAcquireSpinLock(&Disk->Queue.ReadQueueLock, &oldIrql);
        if (!DiskPopReadDispatchLocked(Disk, &slot, &request)) {
            KeReleaseSpinLock(&Disk->Queue.ReadQueueLock, oldIrql);
            break;
        }
        KeReleaseSpinLock(&Disk->Queue.ReadQueueLock, oldIrql);

        event = (PYUMEDISK_READ_SLOT_EVENT)(ULONG_PTR)slot->BufferVa;
        RtlZeroMemory(event, sizeof(*event));
        event->EventId = request->EventId;
        event->TargetId = request->TargetId;
        event->Lba = request->Lba;
        event->BlockCount = request->BlockCount;
        event->DataLength = request->DataLength;
        DiskDebugIncrement(extension, &extension->DebugReadSlotsIssued);

        if (CurrentControlSrb != NULL &&
            CurrentCompleted != NULL &&
            slot->ControlSrb == CurrentControlSrb) {
            *CurrentCompleted = TRUE;
        }

        DiskCompleteSubmitSlotSrb(DeviceExtension, slot->ControlSrb, STATUS_SUCCESS);
        DiskFree(slot);
    }
}

static
VOID
DiskDrainWriteSlots(
    _In_ PVOID DeviceExtension,
    _Inout_ PYUME_DISK Disk,
    _In_opt_ PSTORAGE_REQUEST_BLOCK CurrentControlSrb,
    _Out_opt_ BOOLEAN* CurrentCompleted
)
{
    PDEVICE_CONTEXT extension;

    extension = (PDEVICE_CONTEXT)DeviceExtension;
    for (;;) {
        PDISK_POSTED_WRITE_SLOT slot;
        PDISK_PENDING_WRITE request;
        ULONG seq;
        ULONG byteOffset;
        ULONG dataLength;
        KIRQL oldIrql;
        PYUMEDISK_WRITE_SLOT_HEADER slotHeader;

        KeAcquireSpinLock(&Disk->Queue.WriteQueueLock, &oldIrql);
        if (!DiskPopWriteDispatchLocked(Disk, &slot, &request, &seq, &byteOffset, &dataLength)) {
            KeReleaseSpinLock(&Disk->Queue.WriteQueueLock, oldIrql);
            break;
        }
        KeReleaseSpinLock(&Disk->Queue.WriteQueueLock, oldIrql);

        slotHeader = (PYUMEDISK_WRITE_SLOT_HEADER)(ULONG_PTR)slot->BufferVa;
        RtlZeroMemory(slotHeader, YUMEDISK_WRITE_SLOT_HEADER_BASE_SIZE);
        slotHeader->EventId = request->EventId;
        slotHeader->Seq = seq;
        slotHeader->TotalSeq = request->TotalSeq;
        slotHeader->TargetId = request->TargetId;
        slotHeader->Lba = request->BaseLba + (byteOffset / request->SectorSize);
        slotHeader->ByteOffsetInWrite = byteOffset;
        slotHeader->DataLength = dataLength;
        RtlCopyMemory(
            slotHeader->Data,
            (PUCHAR)request->Srb->DataBuffer + byteOffset,
            dataLength);
        DiskDebugIncrement(extension, &extension->DebugWriteFragmentsIssued);

        if (CurrentControlSrb != NULL &&
            CurrentCompleted != NULL &&
            slot->ControlSrb == CurrentControlSrb) {
            *CurrentCompleted = TRUE;
        }

        DiskCompleteSubmitSlotSrb(DeviceExtension, slot->ControlSrb, STATUS_SUCCESS);
        DiskFree(slot);
    }
}

static
VOID
DiskExtractAllReadSlotsLocked(
    _Inout_ PYUME_DISK Disk,
    _Inout_ PLIST_ENTRY DetachedList
)
{
    PLIST_ENTRY entry;
    PLIST_ENTRY next;

    for (entry = Disk->Queue.PostedReadSlots.Flink;
        entry != &Disk->Queue.PostedReadSlots;
        entry = next) {
        next = entry->Flink;
        RemoveEntryList(entry);
        InsertTailList(DetachedList, entry);
    }
}

static
VOID
DiskExtractAllPendingReadsLocked(
    _Inout_ PYUME_DISK Disk,
    _Inout_ PLIST_ENTRY DetachedList
)
{
    PLIST_ENTRY entry;
    PLIST_ENTRY next;

    for (entry = Disk->Queue.PendingReads.Flink;
        entry != &Disk->Queue.PendingReads;
        entry = next) {
        next = entry->Flink;
        RemoveEntryList(entry);
        InsertTailList(DetachedList, entry);
    }
}

static
VOID
DiskExtractAllWriteSlotsLocked(
    _Inout_ PYUME_DISK Disk,
    _Inout_ PLIST_ENTRY DetachedList
)
{
    PLIST_ENTRY entry;
    PLIST_ENTRY next;

    for (entry = Disk->Queue.PostedWriteSlots.Flink;
        entry != &Disk->Queue.PostedWriteSlots;
        entry = next) {
        next = entry->Flink;
        RemoveEntryList(entry);
        InsertTailList(DetachedList, entry);
    }
}

static
VOID
DiskExtractAllPendingWritesLocked(
    _Inout_ PYUME_DISK Disk,
    _Inout_ PLIST_ENTRY DetachedList
)
{
    PLIST_ENTRY entry;
    PLIST_ENTRY next;

    for (entry = Disk->Queue.PendingWrites.Flink;
        entry != &Disk->Queue.PendingWrites;
        entry = next) {
        next = entry->Flink;
        RemoveEntryList(entry);
        InsertTailList(DetachedList, entry);
    }
}

static
VOID
DiskDetachDiskPending(
    _Inout_ PYUME_DISK Disk,
    _Inout_ PLIST_ENTRY DetachedReadSlots,
    _Inout_ PLIST_ENTRY DetachedReads,
    _Inout_ PLIST_ENTRY DetachedWriteSlots,
    _Inout_ PLIST_ENTRY DetachedWrites
)
{
    KIRQL oldIrql;

    KeAcquireSpinLock(&Disk->Queue.ReadQueueLock, &oldIrql);
    DiskExtractAllReadSlotsLocked(Disk, DetachedReadSlots);
    DiskExtractAllPendingReadsLocked(Disk, DetachedReads);
    KeReleaseSpinLock(&Disk->Queue.ReadQueueLock, oldIrql);

    KeAcquireSpinLock(&Disk->Queue.WriteQueueLock, &oldIrql);
    DiskExtractAllWriteSlotsLocked(Disk, DetachedWriteSlots);
    DiskExtractAllPendingWritesLocked(Disk, DetachedWrites);
    KeReleaseSpinLock(&Disk->Queue.WriteQueueLock, oldIrql);
}

static
VOID
DiskDetachDiskPendingIo(
    _Inout_ PYUME_DISK Disk,
    _Inout_ PLIST_ENTRY DetachedReads,
    _Inout_ PLIST_ENTRY DetachedWrites
)
{
    KIRQL oldIrql;

    KeAcquireSpinLock(&Disk->Queue.ReadQueueLock, &oldIrql);
    DiskExtractAllPendingReadsLocked(Disk, DetachedReads);
    KeReleaseSpinLock(&Disk->Queue.ReadQueueLock, oldIrql);

    KeAcquireSpinLock(&Disk->Queue.WriteQueueLock, &oldIrql);
    DiskExtractAllPendingWritesLocked(Disk, DetachedWrites);
    KeReleaseSpinLock(&Disk->Queue.WriteQueueLock, oldIrql);
}

static
NTSTATUS
DiskApplyWriteAckRange(
    _In_ PVOID DeviceExtension,
    _Inout_ PYUME_DISK Disk,
    _In_ const YUMEDISK_WRITE_ACK_RANGE* Range,
    _Inout_opt_ PLIST_ENTRY CompletedWrites
)
{
    PDEVICE_CONTEXT extension;
    PDISK_PENDING_WRITE request;
    ULONGLONG endSeq64;
    BOOLEAN shouldComplete;
    NTSTATUS completeStatus;
    ULONG transferLength;
    KIRQL oldIrql;
    ULONG seq;

    if (Range->EventId == 0 || Range->SeqCount == 0) {
        return STATUS_INVALID_PARAMETER;
    }

    endSeq64 = (ULONGLONG)Range->SeqBase + Range->SeqCount;
    if (endSeq64 > MAXULONG) {
        return STATUS_INVALID_PARAMETER;
    }

    extension = (PDEVICE_CONTEXT)DeviceExtension;
    shouldComplete = FALSE;
    completeStatus = STATUS_SUCCESS;
    transferLength = 0;

    KeAcquireSpinLock(&Disk->Queue.WriteQueueLock, &oldIrql);
    request = DiskLookupWriteRequestLocked(Disk, Range->EventId);
    if (request == NULL) {
        KeReleaseSpinLock(&Disk->Queue.WriteQueueLock, oldIrql);
        return STATUS_NOT_FOUND;
    }

    if (request->FragmentBytes == 0 ||
        endSeq64 > request->TotalSeq ||
        endSeq64 > request->NextIssueSeq) {
        KeReleaseSpinLock(&Disk->Queue.WriteQueueLock, oldIrql);
        return STATUS_INVALID_PARAMETER;
    }

    if (!NT_SUCCESS(Range->IoStatus)) {
        RemoveEntryList(&request->Link);
        shouldComplete = TRUE;
        completeStatus = Range->IoStatus;
    } else {
        DiskDebugAdd(extension, &extension->DebugWriteAcksApplied, Range->SeqCount);
        for (seq = Range->SeqBase; seq < (ULONG)endSeq64; ++seq) {
            if (!RtlCheckBit(&request->AckedBitmap, seq)) {
                RtlSetBits(&request->AckedBitmap, seq, 1);
                request->AckedCount++;
            }
        }

        if (request->AckedCount == request->TotalSeq) {
            RemoveEntryList(&request->Link);
            shouldComplete = TRUE;
            completeStatus = STATUS_SUCCESS;
            transferLength = request->TotalBytes;
        }
    }
    if (shouldComplete) {
        request->CompletionStatus = completeStatus;
        request->CompletionTransferLength = transferLength;
        if (CompletedWrites != NULL) {
            InsertTailList(CompletedWrites, &request->Link);
        }
    }
    KeReleaseSpinLock(&Disk->Queue.WriteQueueLock, oldIrql);

    if (shouldComplete && CompletedWrites == NULL) {
        if (NT_SUCCESS(request->CompletionStatus)) {
            DiskDebugIncrement(extension, &extension->DebugWriteRequestsCompleted);
        } else {
            DiskDebugIncrement(extension, &extension->DebugWriteRequestsFailed);
        }
        DiskCompleteScsiSrb(
            DeviceExtension,
            request->Srb,
            request->CompletionStatus,
            request->CompletionTransferLength);
        DiskFree(request);
    }

    return STATUS_SUCCESS;
}

VOID
DiskInitializeQueueState(
    _Out_ PDEVICE_CONTEXT Extension
)
{
    ULONG index;

    for (index = 0; index < Extension->MaxTargets; ++index) {
        InitializeListHead(&Extension->Disk[index].Queue.PostedReadSlots);
        InitializeListHead(&Extension->Disk[index].Queue.PendingReads);
        InitializeListHead(&Extension->Disk[index].Queue.PostedWriteSlots);
        InitializeListHead(&Extension->Disk[index].Queue.PendingWrites);
    }
    Extension->NextEventId = 0;
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
    LIST_ENTRY detachedReadSlots;
    LIST_ENTRY detachedReads;
    LIST_ENTRY detachedWriteSlots;
    LIST_ENTRY detachedWrites;

    extension = (PDEVICE_CONTEXT)DeviceExtension;
    disk = DiskGetTargetDisk(extension, TargetId);
    if (disk == NULL) {
        return;
    }
    InitializeListHead(&detachedReadSlots);
    InitializeListHead(&detachedReads);
    InitializeListHead(&detachedWriteSlots);
    InitializeListHead(&detachedWrites);

    DiskDetachDiskPending(
        disk,
        &detachedReadSlots,
        &detachedReads,
        &detachedWriteSlots,
        &detachedWrites);

    DiskCompleteDetachedReadSlots(DeviceExtension, &detachedReadSlots, Status);
    DiskCompleteDetachedReads(DeviceExtension, &detachedReads, Status);
    DiskCompleteDetachedWriteSlots(DeviceExtension, &detachedWriteSlots, Status);
    DiskCompleteDetachedWrites(DeviceExtension, &detachedWrites, Status);
}

VOID
DiskCompleteAllPending(
    _In_ PVOID DeviceExtension,
    _In_ NTSTATUS Status
)
{
    PDEVICE_CONTEXT extension;
    ULONG targetId;

    extension = (PDEVICE_CONTEXT)DeviceExtension;
    for (targetId = YUMEDISK_MIN_TARGET_ID; targetId <= YUMEDISK_MAX_USABLE_TARGET_ID; ++targetId) {
        if (DiskGetTargetDisk(extension, targetId) != NULL) {
            DiskCompleteTargetPending(DeviceExtension, targetId, Status);
        }
    }
}

VOID
DiskCompleteTargetPendingIo(
    _In_ PVOID DeviceExtension,
    _In_ ULONG TargetId,
    _In_ NTSTATUS Status
)
{
    PDEVICE_CONTEXT extension;
    PYUME_DISK disk;
    LIST_ENTRY detachedReads;
    LIST_ENTRY detachedWrites;

    extension = (PDEVICE_CONTEXT)DeviceExtension;
    disk = DiskGetTargetDisk(extension, TargetId);
    if (disk == NULL) {
        return;
    }
    InitializeListHead(&detachedReads);
    InitializeListHead(&detachedWrites);

    DiskDetachDiskPendingIo(
        disk,
        &detachedReads,
        &detachedWrites);

    DiskCompleteDetachedReads(DeviceExtension, &detachedReads, Status);
    DiskCompleteDetachedWrites(DeviceExtension, &detachedWrites, Status);
}

VOID
DiskCompleteAllPendingIo(
    _In_ PVOID DeviceExtension,
    _In_ NTSTATUS Status
)
{
    PDEVICE_CONTEXT extension;
    ULONG targetId;

    extension = (PDEVICE_CONTEXT)DeviceExtension;
    for (targetId = YUMEDISK_MIN_TARGET_ID; targetId <= YUMEDISK_MAX_USABLE_TARGET_ID; ++targetId) {
        if (DiskGetTargetDisk(extension, targetId) != NULL) {
            DiskCompleteTargetPendingIo(DeviceExtension, targetId, Status);
        }
    }
}

NTSTATUS
DiskQueueSubmitSlot(
    _In_ PVOID DeviceExtension,
    _In_ PSTORAGE_REQUEST_BLOCK Srb,
    _Inout_ PYUMEDISK_MESSAGE Message,
    _Out_ BOOLEAN* RequestCompleted
)
{
    PDEVICE_CONTEXT extension;
    PYUME_DISK disk;
    PYUMEDISK_SUBMIT_SLOT submitSlot;

    *RequestCompleted = FALSE;
    extension = (PDEVICE_CONTEXT)DeviceExtension;
    submitSlot = (PYUMEDISK_SUBMIT_SLOT)Message->Payload;
    disk = DiskGetTargetDisk(extension, submitSlot->Slot.TargetId);

    if (disk == NULL || !DiskIsTargetVisible(extension, (UCHAR)submitSlot->Slot.TargetId)) {
        return STATUS_NOT_FOUND;
    }

    if (submitSlot->Slot.SlotType == YumeDiskSlotTypeRead) {
        PDISK_POSTED_READ_SLOT slot;
        KIRQL oldIrql;

        if (submitSlot->Slot.Capacity < sizeof(YUMEDISK_READ_SLOT_EVENT)) {
            return STATUS_BUFFER_TOO_SMALL;
        }

        slot = (PDISK_POSTED_READ_SLOT)DiskAlloc(sizeof(*slot));
        if (slot == NULL) {
            return STATUS_INSUFFICIENT_RESOURCES;
        }

        RtlZeroMemory(slot, sizeof(*slot));
        slot->ControlSrb = Srb;
        slot->SlotId = submitSlot->Slot.SlotId;
        slot->TargetId = submitSlot->Slot.TargetId;
        slot->BufferVa = (PUCHAR)(ULONG_PTR)submitSlot->Slot.KernelVa;
        slot->Capacity = submitSlot->Slot.Capacity;

        KeAcquireSpinLock(&disk->Queue.ReadQueueLock, &oldIrql);
        InsertTailList(&disk->Queue.PostedReadSlots, &slot->Link);
        KeReleaseSpinLock(&disk->Queue.ReadQueueLock, oldIrql);

        DiskDrainReadSlots(DeviceExtension, disk, Srb, RequestCompleted);
        return *RequestCompleted ? STATUS_SUCCESS : STATUS_PENDING;
    }

    {
        PDISK_POSTED_WRITE_SLOT slot;
        ULONG sectorSize;
        KIRQL oldIrql;

        sectorSize = disk->SectorSize;
        if (DiskAlignWritePayloadBytes(submitSlot->Slot.Capacity, sectorSize) == 0) {
            return STATUS_BUFFER_TOO_SMALL;
        }

        slot = (PDISK_POSTED_WRITE_SLOT)DiskAlloc(sizeof(*slot));
        if (slot == NULL) {
            return STATUS_INSUFFICIENT_RESOURCES;
        }

        RtlZeroMemory(slot, sizeof(*slot));
        slot->ControlSrb = Srb;
        slot->SlotId = submitSlot->Slot.SlotId;
        slot->TargetId = submitSlot->Slot.TargetId;
        slot->BufferVa = (PUCHAR)(ULONG_PTR)submitSlot->Slot.KernelVa;
        slot->Capacity = submitSlot->Slot.Capacity;

        KeAcquireSpinLock(&disk->Queue.WriteQueueLock, &oldIrql);
        InsertTailList(&disk->Queue.PostedWriteSlots, &slot->Link);
        KeReleaseSpinLock(&disk->Queue.WriteQueueLock, oldIrql);
    }

    DiskDrainWriteSlots(DeviceExtension, disk, Srb, RequestCompleted);
    return *RequestCompleted ? STATUS_SUCCESS : STATUS_PENDING;
}

NTSTATUS
DiskQueueReadAck(
    _In_ PVOID DeviceExtension,
    _Inout_ PYUMEDISK_MESSAGE Message
)
{
    PDEVICE_CONTEXT extension;
    PYUME_DISK disk;
    PYUMEDISK_READ_ACK readAck;
    PDISK_PENDING_READ request;
    KIRQL oldIrql;
    BOOLEAN shouldComplete;
    NTSTATUS completeStatus;
    ULONG transferLength;

    extension = (PDEVICE_CONTEXT)DeviceExtension;
    disk = DiskGetTargetDisk(extension, Message->Header.TargetId);
    if (disk == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    readAck = (PYUMEDISK_READ_ACK)Message->Payload;
    shouldComplete = FALSE;
    completeStatus = STATUS_SUCCESS;
    transferLength = 0;

    KeAcquireSpinLock(&disk->Queue.ReadQueueLock, &oldIrql);
    request = DiskLookupReadRequestLocked(disk, readAck->EventId);
    if (request == NULL) {
        KeReleaseSpinLock(&disk->Queue.ReadQueueLock, oldIrql);
        return STATUS_NOT_FOUND;
    }

    if (!request->SlotIssued) {
        KeReleaseSpinLock(&disk->Queue.ReadQueueLock, oldIrql);
        return STATUS_INVALID_PARAMETER;
    }

    if (NT_SUCCESS(readAck->IoStatus) && readAck->DataLength != request->DataLength) {
        RemoveEntryList(&request->Link);
        KeReleaseSpinLock(&disk->Queue.ReadQueueLock, oldIrql);
        DiskDebugIncrement(extension, &extension->DebugReadRequestsFailed);
        DiskCompleteScsiSrb(DeviceExtension, request->Srb, STATUS_IO_DEVICE_ERROR, 0);
        DiskFree(request);
        return STATUS_INVALID_PARAMETER;
    }

    RemoveEntryList(&request->Link);
    KeReleaseSpinLock(&disk->Queue.ReadQueueLock, oldIrql);

    if (NT_SUCCESS(readAck->IoStatus) && request->DataLength != 0) {
        RtlCopyMemory(
            request->Srb->DataBuffer,
            (PVOID)(ULONG_PTR)readAck->KernelVa,
            request->DataLength);
    }

    DiskDebugIncrement(extension, &extension->DebugReadAcksApplied);
    if (NT_SUCCESS(readAck->IoStatus)) {
        shouldComplete = TRUE;
        completeStatus = STATUS_SUCCESS;
        transferLength = request->DataLength;
        DiskDebugIncrement(extension, &extension->DebugReadRequestsCompleted);
    } else {
        shouldComplete = TRUE;
        completeStatus = readAck->IoStatus;
        transferLength = 0;
        DiskDebugIncrement(extension, &extension->DebugReadRequestsFailed);
    }

    if (shouldComplete) {
        DiskCompleteScsiSrb(
            DeviceExtension,
            request->Srb,
            completeStatus,
            transferLength);
    }
    DiskFree(request);

    DiskInitMessageStatus(Message, YumeDiskCommandReadAck, STATUS_SUCCESS, 0);
    return STATUS_SUCCESS;
}

NTSTATUS
DiskQueueWriteAckBatch(
    _In_ PVOID DeviceExtension,
    _In_ PSTORAGE_REQUEST_BLOCK Srb,
    _Inout_ PYUMEDISK_MESSAGE Message,
    _Out_ BOOLEAN* RequestCompleted
)
{
    PDEVICE_CONTEXT extension;
    PYUME_DISK disk;
    PYUMEDISK_WRITE_ACK_BATCH batch;
    PYUMEDISK_WRITE_ACK_BATCH_RESULT result;
    PYUMEDISK_WRITE_ACK_FAILURE failures;
    PSRB_IO_CONTROL srbIoControl;
    LIST_ENTRY completedWrites;
    NTSTATUS status;
    ULONG failureCount;
    ULONG responseLength;
    ULONG index;

    *RequestCompleted = FALSE;
    extension = (PDEVICE_CONTEXT)DeviceExtension;
    disk = DiskGetTargetDisk(extension, Message->Header.TargetId);
    if (disk == NULL) {
        return STATUS_INVALID_PARAMETER;
    }

    batch = (PYUMEDISK_WRITE_ACK_BATCH)Message->Payload;
    result = (PYUMEDISK_WRITE_ACK_BATCH_RESULT)Message->Payload;
    failures = result->Failures;
    failureCount = 0;
    InitializeListHead(&completedWrites);

    for (index = 0; index < batch->RangeCount; ++index) {
        status = DiskApplyWriteAckRange(DeviceExtension, disk, &batch->Ranges[index], &completedWrites);
        if (!NT_SUCCESS(status)) {
            failures[failureCount].RangeIndex = index;
            failures[failureCount].Status = status;
            failureCount++;
        }
    }

    responseLength = (ULONG)YUMEDISK_WRITE_ACK_BATCH_RESULT_SIZE(failureCount);
    result->FailureCount = failureCount;
    result->Reserved = 0;
    DiskInitMessageStatus(Message, YumeDiskCommandWriteAckBatch, STATUS_SUCCESS, responseLength);
    if (!DiskGetIoctlResponseBuffers(Srb, &srbIoControl, &Message)) {
        DiskCompleteAckedWrites(DeviceExtension, &completedWrites);
        return STATUS_INVALID_PARAMETER;
    }

    DiskCompleteIoctlSrb(Srb, srbIoControl, STATUS_SUCCESS, Message->Header.Size);
    StorPortNotification(RequestComplete, DeviceExtension, Srb);
    *RequestCompleted = TRUE;
    DiskCompleteAckedWrites(DeviceExtension, &completedWrites);
    return STATUS_SUCCESS;
}

NTSTATUS
DiskQueueCancelSlot(
    _In_ PVOID DeviceExtension,
    _Inout_ PYUMEDISK_MESSAGE Message
)
{
    PDEVICE_CONTEXT extension;
    PYUME_DISK disk;
    PYUMEDISK_CANCEL_SLOT cancelSlot;
    PSTORAGE_REQUEST_BLOCK slotSrb;
    PSTORAGE_REQUEST_BLOCK issuedSrb;
    ULONG transferLength;
    NTSTATUS completeStatus;
    KIRQL oldIrql;

    extension = (PDEVICE_CONTEXT)DeviceExtension;
    cancelSlot = (PYUMEDISK_CANCEL_SLOT)Message->Payload;
    disk = DiskGetTargetDisk(extension, cancelSlot->TargetId);
    if (disk == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    slotSrb = NULL;
    issuedSrb = NULL;
    transferLength = 0;
    completeStatus = STATUS_CANCELLED;

    if (cancelSlot->SlotType == YumeDiskSlotTypeRead) {
        PLIST_ENTRY entry;

        KeAcquireSpinLock(&disk->Queue.ReadQueueLock, &oldIrql);
        for (entry = disk->Queue.PostedReadSlots.Flink;
            entry != &disk->Queue.PostedReadSlots;
            entry = entry->Flink) {
            PDISK_POSTED_READ_SLOT slot;

            slot = CONTAINING_RECORD(entry, DISK_POSTED_READ_SLOT, Link);
            if (slot->SlotId == cancelSlot->SlotId &&
                slot->TargetId == cancelSlot->TargetId) {
                slotSrb = slot->ControlSrb;
                RemoveEntryList(entry);
                DiskFree(slot);
                break;
            }
        }

        if (slotSrb == NULL) {
            for (entry = disk->Queue.PendingReads.Flink;
                entry != &disk->Queue.PendingReads;
                entry = entry->Flink) {
                PDISK_PENDING_READ request;

                request = CONTAINING_RECORD(entry, DISK_PENDING_READ, Link);
                if (!request->SlotIssued || request->IssuedSlotId != cancelSlot->SlotId) {
                    continue;
                }

                issuedSrb = request->Srb;
                RemoveEntryList(entry);
                DiskFree(request);
                break;
            }
        }
        KeReleaseSpinLock(&disk->Queue.ReadQueueLock, oldIrql);
    } else {
        PLIST_ENTRY entry;

        KeAcquireSpinLock(&disk->Queue.WriteQueueLock, &oldIrql);
        for (entry = disk->Queue.PostedWriteSlots.Flink;
            entry != &disk->Queue.PostedWriteSlots;
            entry = entry->Flink) {
            PDISK_POSTED_WRITE_SLOT slot;

            slot = CONTAINING_RECORD(entry, DISK_POSTED_WRITE_SLOT, Link);
            if (slot->SlotId == cancelSlot->SlotId &&
                slot->TargetId == cancelSlot->TargetId) {
                slotSrb = slot->ControlSrb;
                RemoveEntryList(entry);
                DiskFree(slot);
                break;
            }
        }

        if (slotSrb == NULL) {
            for (entry = disk->Queue.PendingWrites.Flink;
                entry != &disk->Queue.PendingWrites;
                entry = entry->Flink) {
                PDISK_PENDING_WRITE request;

                request = CONTAINING_RECORD(entry, DISK_PENDING_WRITE, Link);
                if (request->LastIssuedSlotId != cancelSlot->SlotId) {
                    continue;
                }

                issuedSrb = request->Srb;
                RemoveEntryList(entry);
                DiskFree(request);
                break;
            }
        }
        KeReleaseSpinLock(&disk->Queue.WriteQueueLock, oldIrql);
    }

    if (slotSrb != NULL) {
        DiskCompleteSubmitSlotSrb(DeviceExtension, slotSrb, STATUS_CANCELLED);
    } else if (issuedSrb != NULL) {
        if (cancelSlot->SlotType == YumeDiskSlotTypeRead) {
            DiskDebugIncrement(extension, &extension->DebugReadRequestsFailed);
        } else {
            DiskDebugIncrement(extension, &extension->DebugWriteRequestsFailed);
        }
        DiskCompleteScsiSrb(DeviceExtension, issuedSrb, completeStatus, transferLength);
    } else {
        return STATUS_NOT_FOUND;
    }

    DiskInitMessageStatus(Message, YumeDiskCommandCancelSlot, STATUS_SUCCESS, 0);
    return STATUS_SUCCESS;
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
    PDISK_PENDING_READ request;
    KIRQL oldIrql;

    if (DataLength == 0) {
        return STATUS_SUCCESS;
    }

    extension = (PDEVICE_CONTEXT)DeviceExtension;
    disk = DiskGetTargetDisk(extension, TargetId);
    if (!DiskIsTargetVisible(extension, TargetId)) {
        return STATUS_DEVICE_NOT_CONNECTED;
    }
    if (disk == NULL) {
        return STATUS_INVALID_PARAMETER;
    }

    request = (PDISK_PENDING_READ)DiskAlloc(sizeof(*request));
    if (request == NULL) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    RtlZeroMemory(request, sizeof(*request));
    request->Srb = Srb;
    request->EventId = DiskAllocateEventId(extension);
    request->IssuedSlotId = 0;
    request->TargetId = TargetId;
    request->Lba = Lba;
    request->BlockCount = BlockCount;
    request->DataLength = DataLength;
    request->SlotIssued = FALSE;

    KeAcquireSpinLock(&disk->Queue.ReadQueueLock, &oldIrql);
    InsertTailList(&disk->Queue.PendingReads, &request->Link);
    KeReleaseSpinLock(&disk->Queue.ReadQueueLock, oldIrql);
    DiskDebugIncrement(extension, &extension->DebugReadRequestsQueued);

    DiskDrainReadSlots(DeviceExtension, disk, NULL, NULL);
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
    ULONG sectorSize;
    ULONG maxSeq;
    ULONG bitmapWordCount;
    SIZE_T allocationSize;
    PDISK_PENDING_WRITE request;
    KIRQL oldIrql;

    UNREFERENCED_PARAMETER(BlockCount);

    if (DataLength == 0) {
        return STATUS_SUCCESS;
    }

    extension = (PDEVICE_CONTEXT)DeviceExtension;
    disk = DiskGetTargetDisk(extension, TargetId);
    if (!DiskIsTargetVisible(extension, TargetId)) {
        return STATUS_DEVICE_NOT_CONNECTED;
    }
    if (disk == NULL) {
        return STATUS_INVALID_PARAMETER;
    }

    sectorSize = disk->SectorSize;
    if (sectorSize == 0 ||
        (DataLength % sectorSize) != 0 ||
        Srb->DataBuffer == NULL) {
        return STATUS_INVALID_PARAMETER;
    }

    maxSeq = DataLength / sectorSize;
    bitmapWordCount = max(1u, (maxSeq + (sizeof(ULONG) * 8u) - 1u) / (sizeof(ULONG) * 8u));
    allocationSize = FIELD_OFFSET(DISK_PENDING_WRITE, AckedBits) + (bitmapWordCount * sizeof(ULONG));

    request = (PDISK_PENDING_WRITE)DiskAlloc(allocationSize);
    if (request == NULL) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    RtlZeroMemory(request, allocationSize);
    request->Srb = Srb;
    request->EventId = DiskAllocateEventId(extension);
    request->LastIssuedSlotId = 0;
    request->TargetId = TargetId;
    request->BaseLba = Lba;
    request->TotalBytes = DataLength;
    request->SectorSize = sectorSize;
    request->FragmentBytes = 0;
    request->TotalSeq = 0;
    request->NextIssueSeq = 0;
    request->AckedCount = 0;
    RtlInitializeBitMap(&request->AckedBitmap, request->AckedBits, maxSeq);

    KeAcquireSpinLock(&disk->Queue.WriteQueueLock, &oldIrql);
    InsertTailList(&disk->Queue.PendingWrites, &request->Link);
    KeReleaseSpinLock(&disk->Queue.WriteQueueLock, oldIrql);
    DiskDebugIncrement(extension, &extension->DebugWriteRequestsQueued);

    DiskDrainWriteSlots(DeviceExtension, disk, NULL, NULL);
    return STATUS_PENDING;
}

NTSTATUS
DiskQueryDebugState(
    _In_ PVOID DeviceExtension,
    _Out_ PYUMEDISK_DEBUG_STATE DebugState
)
{
    PDEVICE_CONTEXT extension;
    ULONG targetId;
    KIRQL oldIrql;
    PLIST_ENTRY entry;
    ULONG postedReadSlots;
    ULONG pendingReads;
    ULONG pendingReadsIssued;
    ULONG postedWriteSlots;
    ULONG pendingWrites;
    ULONG pendingWriteFragmentsIssued;
    ULONG pendingWriteFragmentsAcked;

    if (DeviceExtension == NULL || DebugState == NULL) {
        return STATUS_INVALID_PARAMETER;
    }

    extension = (PDEVICE_CONTEXT)DeviceExtension;
    RtlZeroMemory(DebugState, sizeof(*DebugState));

    postedReadSlots = 0;
    pendingReads = 0;
    pendingReadsIssued = 0;
    postedWriteSlots = 0;
    pendingWrites = 0;
    pendingWriteFragmentsIssued = 0;
    pendingWriteFragmentsAcked = 0;

    KeAcquireSpinLock(&extension->SessionLock, &oldIrql);
    DebugState->ActiveSessionId = extension->CurrentSessionId;
    KeReleaseSpinLock(&extension->SessionLock, oldIrql);

    for (targetId = YUMEDISK_MIN_TARGET_ID; targetId <= YUMEDISK_MAX_USABLE_TARGET_ID; ++targetId) {
        PYUME_DISK disk;

        disk = DiskGetTargetDisk(extension, targetId);
        if (disk == NULL) {
            continue;
        }

        KeAcquireSpinLock(&disk->Queue.ReadQueueLock, &oldIrql);
        for (entry = disk->Queue.PostedReadSlots.Flink;
            entry != &disk->Queue.PostedReadSlots;
            entry = entry->Flink) {
            postedReadSlots++;
        }

        for (entry = disk->Queue.PendingReads.Flink;
            entry != &disk->Queue.PendingReads;
            entry = entry->Flink) {
            PDISK_PENDING_READ request;

            pendingReads++;
            request = CONTAINING_RECORD(entry, DISK_PENDING_READ, Link);
            if (request->SlotIssued) {
                pendingReadsIssued++;
            }
        }
        KeReleaseSpinLock(&disk->Queue.ReadQueueLock, oldIrql);

        KeAcquireSpinLock(&disk->Queue.WriteQueueLock, &oldIrql);
        for (entry = disk->Queue.PostedWriteSlots.Flink;
            entry != &disk->Queue.PostedWriteSlots;
            entry = entry->Flink) {
            postedWriteSlots++;
        }

        for (entry = disk->Queue.PendingWrites.Flink;
            entry != &disk->Queue.PendingWrites;
            entry = entry->Flink) {
            PDISK_PENDING_WRITE request;

            pendingWrites++;
            request = CONTAINING_RECORD(entry, DISK_PENDING_WRITE, Link);
            pendingWriteFragmentsIssued += request->NextIssueSeq;
            pendingWriteFragmentsAcked += request->AckedCount;
        }
        KeReleaseSpinLock(&disk->Queue.WriteQueueLock, oldIrql);
    }

    DebugState->ProgressCounter = (UINT64)InterlockedCompareExchange64(&extension->DebugProgressCounter, 0, 0);
    DebugState->ReadRequestsQueued = (UINT64)InterlockedCompareExchange64(&extension->DebugReadRequestsQueued, 0, 0);
    DebugState->ReadSlotsIssued = (UINT64)InterlockedCompareExchange64(&extension->DebugReadSlotsIssued, 0, 0);
    DebugState->ReadAcksApplied = (UINT64)InterlockedCompareExchange64(&extension->DebugReadAcksApplied, 0, 0);
    DebugState->ReadRequestsCompleted = (UINT64)InterlockedCompareExchange64(&extension->DebugReadRequestsCompleted, 0, 0);
    DebugState->ReadRequestsFailed = (UINT64)InterlockedCompareExchange64(&extension->DebugReadRequestsFailed, 0, 0);
    DebugState->WriteRequestsQueued = (UINT64)InterlockedCompareExchange64(&extension->DebugWriteRequestsQueued, 0, 0);
    DebugState->WriteFragmentsIssued = (UINT64)InterlockedCompareExchange64(&extension->DebugWriteFragmentsIssued, 0, 0);
    DebugState->WriteAcksApplied = (UINT64)InterlockedCompareExchange64(&extension->DebugWriteAcksApplied, 0, 0);
    DebugState->WriteRequestsCompleted = (UINT64)InterlockedCompareExchange64(&extension->DebugWriteRequestsCompleted, 0, 0);
    DebugState->WriteRequestsFailed = (UINT64)InterlockedCompareExchange64(&extension->DebugWriteRequestsFailed, 0, 0);
    DebugState->PostedReadSlots = postedReadSlots;
    DebugState->PendingReads = pendingReads;
    DebugState->PendingReadsIssued = pendingReadsIssued;
    DebugState->PostedWriteSlots = postedWriteSlots;
    DebugState->PendingWrites = pendingWrites;
    DebugState->PendingWriteFragmentsIssued = pendingWriteFragmentsIssued;
    DebugState->PendingWriteFragmentsAcked = pendingWriteFragmentsAcked;
    return STATUS_SUCCESS;
}
