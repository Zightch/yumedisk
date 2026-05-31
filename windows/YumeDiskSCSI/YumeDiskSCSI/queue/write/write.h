#pragma once

#include "..\..\core\protocol.h"

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

VOID
DiskCompleteWriteRequest(
    _In_ PVOID DeviceExtension,
    _In_ PYUME_WRITE_REQUEST Request
);

VOID
DiskDrainWriteSlots(
    _In_ PVOID DeviceExtension,
    _In_ UCHAR TargetId
);

NTSTATUS
DiskQueueWriteSrb(
    _In_ PVOID DeviceExtension,
    _In_ PSTORAGE_REQUEST_BLOCK Srb,
    _In_ UCHAR TargetId,
    _In_ UINT64 Lba,
    _In_ ULONG BlockCount,
    _In_ ULONG DataLength
);

NTSTATUS
DiskHandleWriteAckBatchIoctl(
    _In_ PVOID DeviceExtension,
    _Inout_ PYUMEDISK_MESSAGE Message,
    _Inout_ PLIST_ENTRY DeferredWriteCompletions
);

VOID
DiskCompleteDeferredWriteCompletions(
    _In_ PVOID DeviceExtension,
    _Inout_ PLIST_ENTRY DeferredWriteCompletions
);
