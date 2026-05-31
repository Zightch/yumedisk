#pragma once

#include "..\..\core\protocol.h"

typedef struct _YUME_POSTED_SLOT {
    LIST_ENTRY Link;
    PSTORAGE_REQUEST_BLOCK Srb;
    UINT64 SlotId;
    UINT64 KernelVa;
    ULONG Capacity;
    ULONG TargetId;
    ULONG SlotType;
} YUME_POSTED_SLOT, *PYUME_POSTED_SLOT;

VOID
DiskResetWriteSlotShapeLocked(
    _Inout_ PYUME_DISK_QUEUE_STATE Queue
);

ULONG
DiskComputeWritePayloadBytes(
    _In_ ULONG SectorSize,
    _In_ ULONG SlotCapacity
);

PYUME_POSTED_SLOT
DiskAllocPostedSlot(
    _In_ PSTORAGE_REQUEST_BLOCK Srb,
    _In_ const YUMEDISK_SLOT_DESCRIPTOR* Slot
);

PYUME_POSTED_SLOT
DiskRemovePostedSlotByIdLocked(
    _Inout_ PLIST_ENTRY SlotList,
    _In_ UINT64 SlotId
);

NTSTATUS
DiskWriteReadSlotEvent(
    _In_ UINT64 EventId,
    _In_ UINT64 Lba,
    _In_ ULONG BlockCount,
    _In_ ULONG DataLength,
    _In_ PYUME_POSTED_SLOT Slot
);

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
);

VOID
DiskCompleteSlotSrb(
    _In_ PVOID DeviceExtension,
    _In_ PYUME_POSTED_SLOT Slot,
    _In_ NTSTATUS Status
);

VOID
DiskCompleteEventSlotSrb(
    _In_ PVOID DeviceExtension,
    _In_ UCHAR TargetId,
    _In_ PSTORAGE_REQUEST_BLOCK Srb,
    _In_ UINT64 KernelVa,
    _In_ ULONG Capacity,
    _In_ NTSTATUS Status,
    _In_opt_ const YUMEDISK_DISK_EVENT* EventRecord
);
