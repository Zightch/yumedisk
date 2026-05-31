#pragma once

#include "..\..\core\protocol.h"

typedef struct _YUME_READ_REQUEST {
    LIST_ENTRY Link;
    PSTORAGE_REQUEST_BLOCK Srb;
    UINT64 EventId;
    UINT64 Lba;
    ULONG BlockCount;
    ULONG DataLength;
    BOOLEAN SlotIssued;
} YUME_READ_REQUEST, *PYUME_READ_REQUEST;

VOID
DiskCompleteReadRequest(
    _In_ PVOID DeviceExtension,
    _In_ PYUME_READ_REQUEST Request,
    _In_ NTSTATUS Status
);

VOID
DiskDrainReadSlots(
    _In_ PVOID DeviceExtension,
    _In_ UCHAR TargetId
);

NTSTATUS
DiskQueueReadSrb(
    _In_ PVOID DeviceExtension,
    _In_ PSTORAGE_REQUEST_BLOCK Srb,
    _In_ UCHAR TargetId,
    _In_ UINT64 Lba,
    _In_ ULONG BlockCount,
    _In_ ULONG DataLength
);

NTSTATUS
DiskHandleReadAckIoctl(
    _In_ PVOID DeviceExtension,
    _Inout_ PYUMEDISK_MESSAGE Message
);
