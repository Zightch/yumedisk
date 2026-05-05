#include "queue.h"

VOID
DiskFreeQueuedState(
    _In_ PVOID DeviceExtension
)
{
    UNREFERENCED_PARAMETER(DeviceExtension);
}

VOID
DiskCompleteAllPending(
    _In_ PVOID DeviceExtension,
    _In_ NTSTATUS Status
)
{
    UNREFERENCED_PARAMETER(DeviceExtension);
    UNREFERENCED_PARAMETER(Status);
}
