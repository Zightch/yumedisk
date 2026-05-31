#pragma once

#include "..\\..\\core\\protocol.h"

VOID
DiskCompletePendingEventSlot(
    _In_ PVOID DeviceExtension,
    _In_ ULONG TargetId,
    _In_ NTSTATUS Status,
    _In_opt_ const YUMEDISK_DISK_EVENT* EventRecord
);

NTSTATUS
DiskHandleSubmitEventSlotIoctl(
    _In_ PVOID DeviceExtension,
    _In_ PSTORAGE_REQUEST_BLOCK Srb,
    _Inout_ PYUMEDISK_MESSAGE Message
);
