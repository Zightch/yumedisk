#include "queue.h"

#include "..\core\memory.h"

#define DISK_QUEUE_ALL_TARGETS ((ULONG)MAXULONG)

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
    ULONG TargetId;
    UINT64 BaseLba;
    ULONG TotalBytes;
    ULONG SectorSize;
    ULONG FragmentBytes;
    ULONG TotalSeq;
    ULONG NextIssueSeq;
    ULONG AckedCount;
    RTL_BITMAP AckedBitmap;
    ULONG AckedBits[1];
} DISK_PENDING_WRITE, *PDISK_PENDING_WRITE;

static
BOOLEAN
DiskTargetMatches(
    _In_ ULONG FilterTargetId,
    _In_ ULONG ItemTargetId
)
{
    return FilterTargetId == DISK_QUEUE_ALL_TARGETS || FilterTargetId == ItemTargetId;
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
    while (!IsListEmpty(CompletedReads)) {
        PDISK_PENDING_READ request;

        request = CONTAINING_RECORD(RemoveHeadList(CompletedReads), DISK_PENDING_READ, Link);
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
    while (!IsListEmpty(CompletedWrites)) {
        PDISK_PENDING_WRITE request;

        request = CONTAINING_RECORD(RemoveHeadList(CompletedWrites), DISK_PENDING_WRITE, Link);
        DiskCompleteScsiSrb(DeviceExtension, request->Srb, Status, 0);
        DiskFree(request);
    }
}

static
PDISK_PENDING_READ
DiskLookupReadRequestLocked(
    _In_ PDEVICE_CONTEXT Extension,
    _In_ UINT64 EventId
)
{
    PLIST_ENTRY entry;

    for (entry = Extension->PendingReads.Flink;
        entry != &Extension->PendingReads;
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
    _In_ PDEVICE_CONTEXT Extension,
    _In_ UINT64 EventId
)
{
    PLIST_ENTRY entry;

    for (entry = Extension->PendingWrites.Flink;
        entry != &Extension->PendingWrites;
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
    _Inout_ PDEVICE_CONTEXT Extension,
    _Out_ PDISK_POSTED_READ_SLOT* Slot,
    _Out_ PDISK_PENDING_READ* Request
)
{
    PLIST_ENTRY requestEntry;

    for (requestEntry = Extension->PendingReads.Flink;
        requestEntry != &Extension->PendingReads;
        requestEntry = requestEntry->Flink) {
        PDISK_PENDING_READ candidateRequest;
        PLIST_ENTRY slotEntry;

        candidateRequest = CONTAINING_RECORD(requestEntry, DISK_PENDING_READ, Link);
        if (candidateRequest->SlotIssued) {
            continue;
        }

        for (slotEntry = Extension->PostedReadSlots.Flink;
            slotEntry != &Extension->PostedReadSlots;
            slotEntry = slotEntry->Flink) {
            PDISK_POSTED_READ_SLOT candidateSlot;

            candidateSlot = CONTAINING_RECORD(slotEntry, DISK_POSTED_READ_SLOT, Link);
            if (candidateSlot->TargetId != candidateRequest->TargetId) {
                continue;
            }

            RemoveEntryList(&candidateSlot->Link);
            candidateRequest->SlotIssued = TRUE;
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
    _Inout_ PDEVICE_CONTEXT Extension,
    _Out_ PDISK_POSTED_WRITE_SLOT* Slot,
    _Out_ PDISK_PENDING_WRITE* Request,
    _Out_ PULONG Seq,
    _Out_ PULONG ByteOffset,
    _Out_ PULONG DataLength
)
{
    PLIST_ENTRY requestEntry;

    for (requestEntry = Extension->PendingWrites.Flink;
        requestEntry != &Extension->PendingWrites;
        requestEntry = requestEntry->Flink) {
        PDISK_PENDING_WRITE candidateRequest;
        PLIST_ENTRY slotEntry;

        candidateRequest = CONTAINING_RECORD(requestEntry, DISK_PENDING_WRITE, Link);
        for (slotEntry = Extension->PostedWriteSlots.Flink;
            slotEntry != &Extension->PostedWriteSlots;
            slotEntry = slotEntry->Flink) {
            PDISK_POSTED_WRITE_SLOT candidateSlot;
            ULONG slotPayloadBytes;
            ULONGLONG byteOffset64;
            ULONG remainingBytes;
            ULONG fragmentBytes;

            candidateSlot = CONTAINING_RECORD(slotEntry, DISK_POSTED_WRITE_SLOT, Link);
            if (candidateSlot->TargetId != candidateRequest->TargetId) {
                continue;
            }

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

        KeAcquireSpinLock(&extension->ReadQueueLock, &oldIrql);
        if (!DiskPopReadDispatchLocked(extension, &slot, &request)) {
            KeReleaseSpinLock(&extension->ReadQueueLock, oldIrql);
            break;
        }
        KeReleaseSpinLock(&extension->ReadQueueLock, oldIrql);

        event = (PYUMEDISK_READ_SLOT_EVENT)(ULONG_PTR)slot->BufferVa;
        RtlZeroMemory(event, sizeof(*event));
        event->EventId = request->EventId;
        event->TargetId = request->TargetId;
        event->Lba = request->Lba;
        event->BlockCount = request->BlockCount;
        event->DataLength = request->DataLength;

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

        KeAcquireSpinLock(&extension->WriteQueueLock, &oldIrql);
        if (!DiskPopWriteDispatchLocked(extension, &slot, &request, &seq, &byteOffset, &dataLength)) {
            KeReleaseSpinLock(&extension->WriteQueueLock, oldIrql);
            break;
        }
        KeReleaseSpinLock(&extension->WriteQueueLock, oldIrql);

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
DiskExtractMatchingReadSlotsLocked(
    _Inout_ PDEVICE_CONTEXT Extension,
    _In_ ULONG TargetId,
    _Inout_ PLIST_ENTRY DetachedList
)
{
    PLIST_ENTRY entry;
    PLIST_ENTRY next;

    for (entry = Extension->PostedReadSlots.Flink;
        entry != &Extension->PostedReadSlots;
        entry = next) {
        PDISK_POSTED_READ_SLOT slot;

        next = entry->Flink;
        slot = CONTAINING_RECORD(entry, DISK_POSTED_READ_SLOT, Link);
        if (!DiskTargetMatches(TargetId, slot->TargetId)) {
            continue;
        }

        RemoveEntryList(entry);
        InsertTailList(DetachedList, entry);
    }
}

static
VOID
DiskExtractMatchingPendingReadsLocked(
    _Inout_ PDEVICE_CONTEXT Extension,
    _In_ ULONG TargetId,
    _Inout_ PLIST_ENTRY DetachedList
)
{
    PLIST_ENTRY entry;
    PLIST_ENTRY next;

    for (entry = Extension->PendingReads.Flink;
        entry != &Extension->PendingReads;
        entry = next) {
        PDISK_PENDING_READ request;

        next = entry->Flink;
        request = CONTAINING_RECORD(entry, DISK_PENDING_READ, Link);
        if (!DiskTargetMatches(TargetId, request->TargetId)) {
            continue;
        }

        RemoveEntryList(entry);
        InsertTailList(DetachedList, entry);
    }
}

static
VOID
DiskExtractMatchingWriteSlotsLocked(
    _Inout_ PDEVICE_CONTEXT Extension,
    _In_ ULONG TargetId,
    _Inout_ PLIST_ENTRY DetachedList
)
{
    PLIST_ENTRY entry;
    PLIST_ENTRY next;

    for (entry = Extension->PostedWriteSlots.Flink;
        entry != &Extension->PostedWriteSlots;
        entry = next) {
        PDISK_POSTED_WRITE_SLOT slot;

        next = entry->Flink;
        slot = CONTAINING_RECORD(entry, DISK_POSTED_WRITE_SLOT, Link);
        if (!DiskTargetMatches(TargetId, slot->TargetId)) {
            continue;
        }

        RemoveEntryList(entry);
        InsertTailList(DetachedList, entry);
    }
}

static
VOID
DiskExtractMatchingPendingWritesLocked(
    _Inout_ PDEVICE_CONTEXT Extension,
    _In_ ULONG TargetId,
    _Inout_ PLIST_ENTRY DetachedList
)
{
    PLIST_ENTRY entry;
    PLIST_ENTRY next;

    for (entry = Extension->PendingWrites.Flink;
        entry != &Extension->PendingWrites;
        entry = next) {
        PDISK_PENDING_WRITE request;

        next = entry->Flink;
        request = CONTAINING_RECORD(entry, DISK_PENDING_WRITE, Link);
        if (!DiskTargetMatches(TargetId, request->TargetId)) {
            continue;
        }

        RemoveEntryList(entry);
        InsertTailList(DetachedList, entry);
    }
}

static
VOID
DiskDetachMatchingPending(
    _Inout_ PDEVICE_CONTEXT Extension,
    _In_ ULONG TargetId,
    _Inout_ PLIST_ENTRY DetachedReadSlots,
    _Inout_ PLIST_ENTRY DetachedReads,
    _Inout_ PLIST_ENTRY DetachedWriteSlots,
    _Inout_ PLIST_ENTRY DetachedWrites
)
{
    KIRQL oldIrql;

    KeAcquireSpinLock(&Extension->ReadQueueLock, &oldIrql);
    DiskExtractMatchingReadSlotsLocked(Extension, TargetId, DetachedReadSlots);
    DiskExtractMatchingPendingReadsLocked(Extension, TargetId, DetachedReads);
    KeReleaseSpinLock(&Extension->ReadQueueLock, oldIrql);

    KeAcquireSpinLock(&Extension->WriteQueueLock, &oldIrql);
    DiskExtractMatchingWriteSlotsLocked(Extension, TargetId, DetachedWriteSlots);
    DiskExtractMatchingPendingWritesLocked(Extension, TargetId, DetachedWrites);
    KeReleaseSpinLock(&Extension->WriteQueueLock, oldIrql);
}

static
NTSTATUS
DiskApplyWriteAckRange(
    _In_ PVOID DeviceExtension,
    _In_ const YUMEDISK_WRITE_ACK_RANGE* Range
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

    KeAcquireSpinLock(&extension->WriteQueueLock, &oldIrql);
    request = DiskLookupWriteRequestLocked(extension, Range->EventId);
    if (request == NULL) {
        KeReleaseSpinLock(&extension->WriteQueueLock, oldIrql);
        return STATUS_SUCCESS;
    }

    if (request->FragmentBytes == 0 ||
        endSeq64 > request->TotalSeq ||
        endSeq64 > request->NextIssueSeq) {
        KeReleaseSpinLock(&extension->WriteQueueLock, oldIrql);
        return STATUS_INVALID_PARAMETER;
    }

    if (!NT_SUCCESS(Range->IoStatus)) {
        RemoveEntryList(&request->Link);
        shouldComplete = TRUE;
        completeStatus = Range->IoStatus;
    } else {
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
    KeReleaseSpinLock(&extension->WriteQueueLock, oldIrql);

    if (shouldComplete) {
        DiskCompleteScsiSrb(DeviceExtension, request->Srb, completeStatus, transferLength);
        DiskFree(request);
    }

    return STATUS_SUCCESS;
}

VOID
DiskInitializeQueueState(
    _Out_ PDEVICE_CONTEXT Extension
)
{
    InitializeListHead(&Extension->PostedReadSlots);
    InitializeListHead(&Extension->PendingReads);
    InitializeListHead(&Extension->PostedWriteSlots);
    InitializeListHead(&Extension->PendingWrites);
    Extension->NextEventId = 0;
}

VOID
DiskFreeQueuedState(
    _In_ PVOID DeviceExtension
)
{
    PDEVICE_CONTEXT extension;

    extension = (PDEVICE_CONTEXT)DeviceExtension;
    InitializeListHead(&extension->PostedReadSlots);
    InitializeListHead(&extension->PendingReads);
    InitializeListHead(&extension->PostedWriteSlots);
    InitializeListHead(&extension->PendingWrites);
}

VOID
DiskCompleteTargetPending(
    _In_ PVOID DeviceExtension,
    _In_ ULONG TargetId,
    _In_ NTSTATUS Status
)
{
    PDEVICE_CONTEXT extension;
    LIST_ENTRY detachedReadSlots;
    LIST_ENTRY detachedReads;
    LIST_ENTRY detachedWriteSlots;
    LIST_ENTRY detachedWrites;

    extension = (PDEVICE_CONTEXT)DeviceExtension;
    InitializeListHead(&detachedReadSlots);
    InitializeListHead(&detachedReads);
    InitializeListHead(&detachedWriteSlots);
    InitializeListHead(&detachedWrites);

    DiskDetachMatchingPending(
        extension,
        TargetId,
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
    DiskCompleteTargetPending(DeviceExtension, DISK_QUEUE_ALL_TARGETS, Status);
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
    PYUMEDISK_SUBMIT_SLOT submitSlot;

    *RequestCompleted = FALSE;
    extension = (PDEVICE_CONTEXT)DeviceExtension;
    submitSlot = (PYUMEDISK_SUBMIT_SLOT)Message->Payload;

    if (!DiskIsTargetVisible(extension, (UCHAR)submitSlot->Slot.TargetId)) {
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

        KeAcquireSpinLock(&extension->ReadQueueLock, &oldIrql);
        InsertTailList(&extension->PostedReadSlots, &slot->Link);
        KeReleaseSpinLock(&extension->ReadQueueLock, oldIrql);

        DiskDrainReadSlots(DeviceExtension, Srb, RequestCompleted);
        return *RequestCompleted ? STATUS_SUCCESS : STATUS_PENDING;
    }

    if (submitSlot->AckPayloadLength != 0) {
        PYUMEDISK_WRITE_ACK_BATCH batch;
        ULONG index;
        NTSTATUS status;

        batch = (PYUMEDISK_WRITE_ACK_BATCH)submitSlot->AckPayload;
        for (index = 0; index < batch->RangeCount; ++index) {
            status = DiskApplyWriteAckRange(DeviceExtension, &batch->Ranges[index]);
            if (!NT_SUCCESS(status)) {
                return status;
            }
        }
    }

    {
        PDISK_POSTED_WRITE_SLOT slot;
        ULONG sectorSize;
        KIRQL oldIrql;

        sectorSize = extension->Disk[submitSlot->Slot.TargetId].SectorSize;
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

        KeAcquireSpinLock(&extension->WriteQueueLock, &oldIrql);
        InsertTailList(&extension->PostedWriteSlots, &slot->Link);
        KeReleaseSpinLock(&extension->WriteQueueLock, oldIrql);
    }

    DiskDrainWriteSlots(DeviceExtension, Srb, RequestCompleted);
    return *RequestCompleted ? STATUS_SUCCESS : STATUS_PENDING;
}

NTSTATUS
DiskQueueReadAck(
    _In_ PVOID DeviceExtension,
    _Inout_ PYUMEDISK_MESSAGE Message
)
{
    PDEVICE_CONTEXT extension;
    PYUMEDISK_READ_ACK readAck;
    PDISK_PENDING_READ request;
    KIRQL oldIrql;

    extension = (PDEVICE_CONTEXT)DeviceExtension;
    readAck = (PYUMEDISK_READ_ACK)Message->Payload;

    KeAcquireSpinLock(&extension->ReadQueueLock, &oldIrql);
    request = DiskLookupReadRequestLocked(extension, readAck->EventId);
    if (request == NULL) {
        KeReleaseSpinLock(&extension->ReadQueueLock, oldIrql);
        return STATUS_NOT_FOUND;
    }

    if (!request->SlotIssued) {
        KeReleaseSpinLock(&extension->ReadQueueLock, oldIrql);
        return STATUS_INVALID_PARAMETER;
    }

    if (NT_SUCCESS(readAck->IoStatus) && readAck->DataLength != request->DataLength) {
        KeReleaseSpinLock(&extension->ReadQueueLock, oldIrql);
        return STATUS_INVALID_PARAMETER;
    }

    RemoveEntryList(&request->Link);
    KeReleaseSpinLock(&extension->ReadQueueLock, oldIrql);

    if (NT_SUCCESS(readAck->IoStatus) && request->DataLength != 0) {
        RtlCopyMemory(
            request->Srb->DataBuffer,
            (PVOID)(ULONG_PTR)readAck->KernelVa,
            request->DataLength);
    }

    DiskCompleteScsiSrb(
        DeviceExtension,
        request->Srb,
        readAck->IoStatus,
        NT_SUCCESS(readAck->IoStatus) ? request->DataLength : 0);
    DiskFree(request);

    DiskInitMessageStatus(Message, YumeDiskCommandReadAck, STATUS_SUCCESS, 0);
    return STATUS_SUCCESS;
}

NTSTATUS
DiskQueueWriteAckBatch(
    _In_ PVOID DeviceExtension,
    _Inout_ PYUMEDISK_MESSAGE Message
)
{
    PYUMEDISK_WRITE_ACK_BATCH batch;
    NTSTATUS status;
    ULONG index;

    batch = (PYUMEDISK_WRITE_ACK_BATCH)Message->Payload;
    for (index = 0; index < batch->RangeCount; ++index) {
        status = DiskApplyWriteAckRange(DeviceExtension, &batch->Ranges[index]);
        if (!NT_SUCCESS(status)) {
            return status;
        }
    }

    DiskInitMessageStatus(Message, YumeDiskCommandWriteAckBatch, STATUS_SUCCESS, 0);
    return STATUS_SUCCESS;
}

NTSTATUS
DiskQueueCancelSlot(
    _In_ PVOID DeviceExtension,
    _Inout_ PYUMEDISK_MESSAGE Message
)
{
    PDEVICE_CONTEXT extension;
    PYUMEDISK_CANCEL_SLOT cancelSlot;
    PSTORAGE_REQUEST_BLOCK slotSrb;
    KIRQL oldIrql;

    extension = (PDEVICE_CONTEXT)DeviceExtension;
    cancelSlot = (PYUMEDISK_CANCEL_SLOT)Message->Payload;
    slotSrb = NULL;

    if (cancelSlot->SlotType == YumeDiskSlotTypeRead) {
        PLIST_ENTRY entry;

        KeAcquireSpinLock(&extension->ReadQueueLock, &oldIrql);
        for (entry = extension->PostedReadSlots.Flink;
            entry != &extension->PostedReadSlots;
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
        KeReleaseSpinLock(&extension->ReadQueueLock, oldIrql);
    } else {
        PLIST_ENTRY entry;

        KeAcquireSpinLock(&extension->WriteQueueLock, &oldIrql);
        for (entry = extension->PostedWriteSlots.Flink;
            entry != &extension->PostedWriteSlots;
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
        KeReleaseSpinLock(&extension->WriteQueueLock, oldIrql);
    }

    if (slotSrb == NULL) {
        return STATUS_NOT_FOUND;
    }

    DiskCompleteSubmitSlotSrb(DeviceExtension, slotSrb, STATUS_CANCELLED);
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
    PDISK_PENDING_READ request;
    KIRQL oldIrql;

    if (DataLength == 0) {
        return STATUS_SUCCESS;
    }

    extension = (PDEVICE_CONTEXT)DeviceExtension;
    if (!DiskIsTargetVisible(extension, TargetId)) {
        return STATUS_DEVICE_NOT_CONNECTED;
    }

    request = (PDISK_PENDING_READ)DiskAlloc(sizeof(*request));
    if (request == NULL) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    RtlZeroMemory(request, sizeof(*request));
    request->Srb = Srb;
    request->EventId = DiskAllocateEventId(extension);
    request->TargetId = TargetId;
    request->Lba = Lba;
    request->BlockCount = BlockCount;
    request->DataLength = DataLength;
    request->SlotIssued = FALSE;

    KeAcquireSpinLock(&extension->ReadQueueLock, &oldIrql);
    InsertTailList(&extension->PendingReads, &request->Link);
    KeReleaseSpinLock(&extension->ReadQueueLock, oldIrql);

    DiskDrainReadSlots(DeviceExtension, NULL, NULL);
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
    if (!DiskIsTargetVisible(extension, TargetId)) {
        return STATUS_DEVICE_NOT_CONNECTED;
    }

    sectorSize = extension->Disk[TargetId].SectorSize;
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
    request->TargetId = TargetId;
    request->BaseLba = Lba;
    request->TotalBytes = DataLength;
    request->SectorSize = sectorSize;
    request->FragmentBytes = 0;
    request->TotalSeq = 0;
    request->NextIssueSeq = 0;
    request->AckedCount = 0;
    RtlInitializeBitMap(&request->AckedBitmap, request->AckedBits, maxSeq);

    KeAcquireSpinLock(&extension->WriteQueueLock, &oldIrql);
    InsertTailList(&extension->PendingWrites, &request->Link);
    KeReleaseSpinLock(&extension->WriteQueueLock, oldIrql);

    DiskDrainWriteSlots(DeviceExtension, NULL, NULL);
    return STATUS_PENDING;
}
