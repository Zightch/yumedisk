#include "slot.h"

#include "..\..\core\memory.h"

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

VOID
DiskResetWriteSlotShapeLocked(
    _Inout_ PYUME_DISK_QUEUE_STATE Queue
)
{
    if (Queue->PostedWriteSlotCount == 0 && Queue->PendingWriteCount == 0) {
        Queue->WriteSlotPayloadBytes = 0;
    }
}

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

PYUME_POSTED_SLOT
DiskRemovePostedSlotByIdLocked(
    _Inout_ PLIST_ENTRY SlotList,
    _In_ UINT64 SlotId
)
{
    PLIST_ENTRY entry;

    for (entry = SlotList->Flink; entry != SlotList; entry = entry->Flink) {
        PYUME_POSTED_SLOT slot;

        slot = CONTAINING_RECORD(entry, YUME_POSTED_SLOT, Link);
        if (slot->SlotId == SlotId) {
            RemoveEntryList(&slot->Link);
            return slot;
        }
    }

    return NULL;
}

NTSTATUS
DiskWriteReadSlotEvent(
    _In_ UINT64 EventId,
    _In_ UINT64 Lba,
    _In_ ULONG BlockCount,
    _In_ ULONG DataLength,
    _In_ PYUME_POSTED_SLOT Slot
)
{
    PYUMEDISK_READ_SLOT_EVENT readEvent;

    if (Slot->KernelVa == 0 || Slot->Capacity < sizeof(*readEvent)) {
        return STATUS_BUFFER_TOO_SMALL;
    }

    readEvent = (PYUMEDISK_READ_SLOT_EVENT)(ULONG_PTR)Slot->KernelVa;
    readEvent->EventId = EventId;
    readEvent->TargetId = Slot->TargetId;
    readEvent->Reserved0 = 0;
    readEvent->Lba = Lba;
    readEvent->BlockCount = BlockCount;
    readEvent->DataLength = DataLength;
    return STATUS_SUCCESS;
}

NTSTATUS
DiskWriteWriteSlotPayload(
    _In_reads_bytes_(TotalBytes) const VOID* SourceBuffer,
    _In_ UINT64 EventId,
    _In_ UINT64 BaseLba,
    _In_ ULONG TotalBytes,
    _In_ ULONG PayloadBytes,
    _In_ ULONG TotalSeq,
    _In_ ULONG Seq,
    _In_ ULONG SectorSize,
    _In_ PYUME_POSTED_SLOT Slot
)
{
    PYUMEDISK_WRITE_SLOT_HEADER header;
    ULONG byteOffset;
    ULONG remainingBytes;
    ULONG fragmentBytes;

    if (Slot->KernelVa == 0 || PayloadBytes == 0 || TotalSeq == 0 || SectorSize == 0) {
        return STATUS_INVALID_PARAMETER;
    }

    byteOffset = Seq * PayloadBytes;
    remainingBytes = TotalBytes - byteOffset;
    fragmentBytes = (remainingBytes < PayloadBytes) ? remainingBytes : PayloadBytes;
    if (Slot->Capacity < YUMEDISK_WRITE_SLOT_HEADER_BASE_SIZE + fragmentBytes) {
        return STATUS_BUFFER_TOO_SMALL;
    }

    header = (PYUMEDISK_WRITE_SLOT_HEADER)(ULONG_PTR)Slot->KernelVa;
    header->EventId = EventId;
    header->Seq = Seq;
    header->TotalSeq = TotalSeq;
    header->TargetId = Slot->TargetId;
    header->Reserved0 = 0;
    header->Lba = BaseLba + (byteOffset / SectorSize);
    header->ByteOffsetInWrite = byteOffset;
    header->DataLength = fragmentBytes;
    header->Flags = 0;
    header->Reserved1 = 0;
    RtlCopyMemory(
        header->Data,
        (const PUCHAR)SourceBuffer + byteOffset,
        fragmentBytes);
    return STATUS_SUCCESS;
}

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

VOID
DiskCompleteEventSlotSrb(
    _In_ PVOID DeviceExtension,
    _In_ UCHAR TargetId,
    _In_ PSTORAGE_REQUEST_BLOCK Srb,
    _In_ UINT64 KernelVa,
    _In_ ULONG Capacity,
    _In_ NTSTATUS Status,
    _In_opt_ const YUMEDISK_DISK_EVENT* EventRecord
)
{
    PSRB_IO_CONTROL srbIoControl;
    PYUMEDISK_MESSAGE message;

    srbIoControl = DiskGetIoctlControl(Srb);
    message = DiskGetIoctlMessage(Srb);

    if (NT_SUCCESS(Status)) {
        if (KernelVa == 0 ||
            Capacity < sizeof(YUMEDISK_DISK_EVENT) ||
            EventRecord == NULL) {
            Status = STATUS_DEVICE_PROTOCOL_ERROR;
        } else {
            RtlCopyMemory(
                (PVOID)(ULONG_PTR)KernelVa,
                EventRecord,
                sizeof(YUMEDISK_DISK_EVENT));
        }
    }

    DiskInitMessageStatus(message, YumeDiskCommandSubmitEventSlot, Status, 0);
    message->Header.TargetId = TargetId;
    DiskCompleteIoctlSrb(Srb, srbIoControl, Status, message->Header.Size);
    StorPortNotification(RequestComplete, DeviceExtension, Srb);
}
