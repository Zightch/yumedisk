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

VOID
DiskCompleteScsiSrb(
    _In_ PVOID DeviceExtension,
    _In_ PSTORAGE_REQUEST_BLOCK Srb,
    _In_ NTSTATUS Status,
    _In_ ULONG DataTransferLength
);

NTSTATUS
DiskClaimSessionLocked(
    _Inout_ PDEVICE_CONTEXT Extension,
    _In_ const YUMEDISK_HEADER* Header
);

VOID
DiskResetDiskStorage(
    _Inout_ PYUME_DISK Disk
);
