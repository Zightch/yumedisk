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
