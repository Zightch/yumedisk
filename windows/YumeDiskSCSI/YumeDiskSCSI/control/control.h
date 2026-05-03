#pragma once

#include "..\queue\queue.h"

BOOLEAN
DiskHandleIoControlSrb(
    _In_ PVOID DeviceExtension,
    _In_ PSTORAGE_REQUEST_BLOCK Srb
);

