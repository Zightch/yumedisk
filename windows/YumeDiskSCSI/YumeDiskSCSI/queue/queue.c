#include "queue.h"

VOID
DiskInitializeQueueState(
    _Out_ PDEVICE_CONTEXT Extension
)
{
    ULONG index;

    Extension->NextEventId = 0;
    Extension->DebugProgressCounter = 0;
    Extension->DebugReadRequestsQueued = 0;
    Extension->DebugReadSlotsIssued = 0;
    Extension->DebugReadAcksApplied = 0;
    Extension->DebugReadRequestsCompleted = 0;
    Extension->DebugReadRequestsFailed = 0;
    Extension->DebugWriteRequestsQueued = 0;
    Extension->DebugWriteFragmentsIssued = 0;
    Extension->DebugWriteAcksApplied = 0;
    Extension->DebugWriteRequestsCompleted = 0;
    Extension->DebugWriteRequestsFailed = 0;

    for (index = 0; index < Extension->MaxTargets; ++index) {
        InitializeListHead(&Extension->Disk[index].Queue.PostedReadSlots);
        InitializeListHead(&Extension->Disk[index].Queue.PendingReads);
        InitializeListHead(&Extension->Disk[index].Queue.PostedWriteSlots);
        InitializeListHead(&Extension->Disk[index].Queue.PendingWrites);
    }
}

VOID
DiskFreeQueuedState(
    _In_ PVOID DeviceExtension
)
{
    PDEVICE_CONTEXT extension;
    ULONG index;

    extension = (PDEVICE_CONTEXT)DeviceExtension;
    for (index = 0; index < extension->MaxTargets; ++index) {
        InitializeListHead(&extension->Disk[index].Queue.PostedReadSlots);
        InitializeListHead(&extension->Disk[index].Queue.PendingReads);
        InitializeListHead(&extension->Disk[index].Queue.PostedWriteSlots);
        InitializeListHead(&extension->Disk[index].Queue.PendingWrites);
    }
}

VOID
DiskCompleteTargetPending(
    _In_ PVOID DeviceExtension,
    _In_ ULONG TargetId,
    _In_ NTSTATUS Status
)
{
    UNREFERENCED_PARAMETER(DeviceExtension);
    UNREFERENCED_PARAMETER(TargetId);
    UNREFERENCED_PARAMETER(Status);
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

VOID
DiskCompleteTargetPendingIo(
    _In_ PVOID DeviceExtension,
    _In_ ULONG TargetId,
    _In_ NTSTATUS Status
)
{
    DiskCompleteTargetPending(DeviceExtension, TargetId, Status);
}

VOID
DiskCompleteAllPendingIo(
    _In_ PVOID DeviceExtension,
    _In_ NTSTATUS Status
)
{
    DiskCompleteAllPending(DeviceExtension, Status);
}

NTSTATUS
DiskQueueReadSrb(
    _In_ PVOID DeviceExtension,
    _In_ PSTORAGE_REQUEST_BLOCK Srb,
    _In_ UCHAR TargetId,
    _In_ UINT64 Lba,
    _In_ ULONG BlockCount,
    _In_ ULONG DataLength
)
{
    UNREFERENCED_PARAMETER(DeviceExtension);
    UNREFERENCED_PARAMETER(Srb);
    UNREFERENCED_PARAMETER(TargetId);
    UNREFERENCED_PARAMETER(Lba);
    UNREFERENCED_PARAMETER(BlockCount);
    UNREFERENCED_PARAMETER(DataLength);
    return STATUS_NOT_SUPPORTED;
}

NTSTATUS
DiskQueueWriteSrb(
    _In_ PVOID DeviceExtension,
    _In_ PSTORAGE_REQUEST_BLOCK Srb,
    _In_ UCHAR TargetId,
    _In_ UINT64 Lba,
    _In_ ULONG BlockCount,
    _In_ ULONG DataLength
)
{
    UNREFERENCED_PARAMETER(DeviceExtension);
    UNREFERENCED_PARAMETER(Srb);
    UNREFERENCED_PARAMETER(TargetId);
    UNREFERENCED_PARAMETER(Lba);
    UNREFERENCED_PARAMETER(BlockCount);
    UNREFERENCED_PARAMETER(DataLength);
    return STATUS_NOT_SUPPORTED;
}

NTSTATUS
DiskQueryDebugState(
    _In_ PVOID DeviceExtension,
    _Out_ PYUMEDISK_DEBUG_STATE DebugState
)
{
    PDEVICE_CONTEXT extension;
    KIRQL oldIrql;

    extension = (PDEVICE_CONTEXT)DeviceExtension;
    RtlZeroMemory(DebugState, sizeof(*DebugState));

    KeAcquireSpinLock(&extension->SessionLock, &oldIrql);
    DebugState->ActiveSessionId = extension->CurrentSessionId;
    KeReleaseSpinLock(&extension->SessionLock, oldIrql);

    return STATUS_SUCCESS;
}
