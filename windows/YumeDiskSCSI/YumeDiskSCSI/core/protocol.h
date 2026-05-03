#pragma once

#include "defs.h"

BOOLEAN
DiskIsUsableTargetId(
    _In_ ULONG TargetId
);

BOOLEAN
DiskIsTargetVisible(
    _In_ PDEVICE_CONTEXT Extension,
    _In_ UCHAR TargetId
);

VOID
DiskInitMessageStatus(
    _Inout_ PYUMEDISK_MESSAGE Message,
    _In_ ULONG Command,
    _In_ NTSTATUS Status,
    _In_ ULONG PayloadLength
);

VOID
DiskCompleteIoctlSrb(
    _In_ PSTORAGE_REQUEST_BLOCK Srb,
    _In_ PSRB_IO_CONTROL SrbIoControl,
    _In_ NTSTATUS Status,
    _In_ ULONG ResponseLength
);

ULONGLONG
DiskAllocateTxIdLocked(
    _Inout_ PDEVICE_CONTEXT Extension
);

VOID
DiskAssignEventTxId(
    _In_ PDEVICE_CONTEXT Extension,
    _Inout_ YUMEDISK_EVENT* Event
);

NTSTATUS
DiskClaimSessionLocked(
    _Inout_ PDEVICE_CONTEXT Extension,
    _In_ const YUMEDISK_HEADER* Header
);

PSRBEX_DATA_SCSI_CDB16
DiskGetScsiCdb16Data(
    _In_ PSTORAGE_REQUEST_BLOCK Srb
);

UCHAR
DiskNtStatusToSrbStatus(
    _In_ NTSTATUS Status
);

VOID
DiskCompleteScsiSrb(
    _In_ PVOID DeviceExtension,
    _In_ PYUMEDISK_PENDING_IO_NODE PendingIo,
    _In_ NTSTATUS Status,
    _In_reads_bytes_opt_(DataLength) const VOID* Data,
    _In_ ULONG DataLength
);

BOOLEAN
DiskValidateDiskRange(
    _In_ PYUME_DISK Disk,
    _In_ UINT64 StartBlockIndex,
    _In_ ULONG BlockCount,
    _In_ ULONG TransferLength,
    _Out_ UINT64* StartByte,
    _Out_ UINT64* ByteCount
);

VOID
DiskResetDiskStorage(
    _Inout_ PYUME_DISK Disk
);

