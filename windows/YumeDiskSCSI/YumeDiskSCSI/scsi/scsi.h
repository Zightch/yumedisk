#pragma once

#include "..\queue\queue.h"

VOID
DiskHandleScsiCdb(
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

