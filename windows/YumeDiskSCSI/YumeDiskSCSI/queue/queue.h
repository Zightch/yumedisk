#pragma once

#include "..\core\protocol.h"

VOID
DiskFreeQueuedState(
    _In_ PVOID DeviceExtension
);

VOID
DiskCompleteAllPending(
    _In_ PVOID DeviceExtension,
    _In_ NTSTATUS Status
);

VOID
DiskQueueSyntheticEvent(
    _In_ PVOID DeviceExtension,
    _In_ ULONG EventType,
    _In_ ULONG TargetId
);

NTSTATUS
DiskQueuePendingScsiIo(
    _In_ PVOID DeviceExtension,
    _Inout_ PDEVICE_CONTEXT Extension,
    _In_ PSTORAGE_REQUEST_BLOCK Srb,
    _In_ ULONG Type,
    _In_ ULONG TargetId,
    _In_ ULONGLONG Lba,
    _In_ ULONG BlockCount,
    _In_ ULONG DataLength
);

VOID
DiskCancelPendingIoByTarget(
    _In_ PVOID DeviceExtension,
    _Inout_ PDEVICE_CONTEXT Extension,
    _In_ ULONG TargetId,
    _In_ NTSTATUS Status
);

NTSTATUS
DiskHandleReadReply(
    _In_ PVOID DeviceExtension,
    _Inout_ PDEVICE_CONTEXT Extension,
    _Inout_ PYUMEDISK_MESSAGE Message
);

NTSTATUS
DiskHandleWriteAck(
    _In_ PVOID DeviceExtension,
    _Inout_ PDEVICE_CONTEXT Extension,
    _Inout_ PYUMEDISK_MESSAGE Message
);

NTSTATUS
DiskHandleWaitEvent(
    _In_ PVOID DeviceExtension,
    _Inout_ PDEVICE_CONTEXT Extension,
    _Inout_ PYUMEDISK_MESSAGE Message,
    _In_ PSTORAGE_REQUEST_BLOCK Srb
);

