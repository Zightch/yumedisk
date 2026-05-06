#pragma once

#include "..\core\protocol.h"

VOID
DiskInitializeQueueState(
    _Out_ PDEVICE_CONTEXT Extension
);

VOID
DiskFreeQueuedState(
    _In_ PVOID DeviceExtension
);

VOID
DiskCompleteTargetPending(
    _In_ PVOID DeviceExtension,
    _In_ ULONG TargetId,
    _In_ NTSTATUS Status
);

VOID
DiskCompleteAllPending(
    _In_ PVOID DeviceExtension,
    _In_ NTSTATUS Status
);

VOID
DiskCompleteTargetPendingIo(
    _In_ PVOID DeviceExtension,
    _In_ ULONG TargetId,
    _In_ NTSTATUS Status
);

VOID
DiskCompleteAllPendingIo(
    _In_ PVOID DeviceExtension,
    _In_ NTSTATUS Status
);

NTSTATUS
DiskQueueSubmitSlot(
    _In_ PVOID DeviceExtension,
    _In_ PSTORAGE_REQUEST_BLOCK Srb,
    _Inout_ PYUMEDISK_MESSAGE Message,
    _Out_ BOOLEAN* RequestCompleted
);

NTSTATUS
DiskQueueReadAck(
    _In_ PVOID DeviceExtension,
    _Inout_ PYUMEDISK_MESSAGE Message
);

NTSTATUS
DiskQueueWriteAckBatch(
    _In_ PVOID DeviceExtension,
    _Inout_ PYUMEDISK_MESSAGE Message
);

NTSTATUS
DiskQueueCancelSlot(
    _In_ PVOID DeviceExtension,
    _Inout_ PYUMEDISK_MESSAGE Message
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
DiskQueueWriteSrb(
    _In_ PVOID DeviceExtension,
    _In_ PSTORAGE_REQUEST_BLOCK Srb,
    _In_ UCHAR TargetId,
    _In_ UINT64 Lba,
    _In_ ULONG BlockCount,
    _In_ ULONG DataLength
);

NTSTATUS
DiskQueryDebugState(
    _In_ PVOID DeviceExtension,
    _Out_ PYUMEDISK_DEBUG_STATE DebugState
);
