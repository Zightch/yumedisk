#pragma once

#include <ntddk.h>
#include <storport.h>

typedef struct _YUME_DISK YUME_DISK, *PYUME_DISK;

VOID YumeDiskResetDiskStorage(
    _Inout_ PYUME_DISK Disk
);

VOID YumeDiskCompleteAllPending(
    _In_ PVOID DeviceExtension,
    _In_ NTSTATUS Status
);

VOID YumeDiskFreeQueuedState(
    _In_ PVOID DeviceExtension
);

VOID YumeDiskQueueSyntheticEvent(
    _In_ PVOID DeviceExtension,
    _In_ ULONG EventType,
    _In_ ULONG TargetId
);

VOID YumeDiskHandleScsiCdb(
    _In_ PVOID DeviceExtension,
    _In_ PSTORAGE_REQUEST_BLOCK Srb,
    _In_ UCHAR TargetId,
    _Inout_ UCHAR* SrbStatus,
    _Inout_ UCHAR* ScsiStatus,
    _Inout_updates_bytes_(*DataTransferLength) PUCHAR DataBuffer,
    _Inout_ ULONG* DataTransferLength,
    _Inout_updates_bytes_opt_(*SenseInfoBufferLength) PUCHAR SenseInfoBuffer,
    _Inout_ UCHAR* SenseInfoBufferLength,
    _In_ PCDB Cdb
);

BOOLEAN YumeDiskHandleIoControlSrb(
    _In_ PVOID DeviceExtension,
    _In_ PSTORAGE_REQUEST_BLOCK Srb
);
