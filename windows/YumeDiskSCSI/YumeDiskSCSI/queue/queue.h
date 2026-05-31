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
DiskHandleSubmitSlotIoctl(
    _In_ PVOID DeviceExtension,
    _In_ PSTORAGE_REQUEST_BLOCK Srb,
    _Inout_ PYUMEDISK_MESSAGE Message
);

NTSTATUS
DiskHandleSubmitEventSlotIoctl(
    _In_ PVOID DeviceExtension,
    _In_ PSTORAGE_REQUEST_BLOCK Srb,
    _Inout_ PYUMEDISK_MESSAGE Message
);

NTSTATUS
DiskHandleReadAckIoctl(
    _In_ PVOID DeviceExtension,
    _Inout_ PYUMEDISK_MESSAGE Message
);

NTSTATUS
DiskHandleWriteAckBatchIoctl(
    _In_ PVOID DeviceExtension,
    _Inout_ PYUMEDISK_MESSAGE Message,
    _Inout_ PLIST_ENTRY DeferredWriteCompletions
);

NTSTATUS
DiskHandleCancelSlotIoctl(
    _In_ PVOID DeviceExtension,
    _Inout_ PYUMEDISK_MESSAGE Message
);

VOID
DiskCompleteDeferredWriteCompletions(
    _In_ PVOID DeviceExtension,
    _Inout_ PLIST_ENTRY DeferredWriteCompletions
);

NTSTATUS
DiskQueryDebugState(
    _In_ PVOID DeviceExtension,
    _Out_ PYUMEDISK_DEBUG_STATE DebugState
);
