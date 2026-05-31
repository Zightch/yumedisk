#include "debug.h"

#include "..\\write\\write.h"

NTSTATUS
DiskQueryDebugState(
    _In_ PVOID DeviceExtension,
    _Out_ PYUMEDISK_DEBUG_STATE DebugState
)
{
    PDEVICE_CONTEXT extension;
    ULONG index;
    KIRQL oldIrql;

    extension = (PDEVICE_CONTEXT)DeviceExtension;
    RtlZeroMemory(DebugState, sizeof(*DebugState));

    KeAcquireSpinLock(&extension->SessionLock, &oldIrql);
    DebugState->ActiveSessionId = extension->CurrentSessionId;
    KeReleaseSpinLock(&extension->SessionLock, oldIrql);
    DebugState->ProgressCounter = InterlockedCompareExchange64(&extension->DebugProgressCounter, 0, 0);
    DebugState->ReadRequestsQueued = InterlockedCompareExchange64(&extension->DebugReadRequestsQueued, 0, 0);
    DebugState->ReadSlotsIssued = InterlockedCompareExchange64(&extension->DebugReadSlotsIssued, 0, 0);
    DebugState->ReadAcksApplied = InterlockedCompareExchange64(&extension->DebugReadAcksApplied, 0, 0);
    DebugState->ReadRequestsCompleted = InterlockedCompareExchange64(&extension->DebugReadRequestsCompleted, 0, 0);
    DebugState->ReadRequestsFailed = InterlockedCompareExchange64(&extension->DebugReadRequestsFailed, 0, 0);
    DebugState->WriteRequestsQueued = InterlockedCompareExchange64(&extension->DebugWriteRequestsQueued, 0, 0);
    DebugState->WriteFragmentsIssued = InterlockedCompareExchange64(&extension->DebugWriteFragmentsIssued, 0, 0);
    DebugState->WriteAcksApplied = InterlockedCompareExchange64(&extension->DebugWriteAcksApplied, 0, 0);
    DebugState->WriteRequestsCompleted = InterlockedCompareExchange64(&extension->DebugWriteRequestsCompleted, 0, 0);
    DebugState->WriteRequestsFailed = InterlockedCompareExchange64(&extension->DebugWriteRequestsFailed, 0, 0);

    for (index = 0; index < extension->MaxTargets; ++index) {
        PYUME_DISK_QUEUE_STATE queue;

        queue = &extension->Disk[index].Queue;

        KeAcquireSpinLock(&queue->ReadQueueLock, &oldIrql);
        DebugState->PostedReadSlots += queue->PostedReadSlotCount;
        DebugState->PendingReads += queue->PendingReadCount;
        DebugState->PendingReadsIssued += queue->PendingReadIssuedCount;
        KeReleaseSpinLock(&queue->ReadQueueLock, oldIrql);

        KeAcquireSpinLock(&queue->WriteQueueLock, &oldIrql);
        {
            PLIST_ENTRY entry;

            DebugState->PostedWriteSlots += queue->PostedWriteSlotCount;
            DebugState->PendingWrites += queue->PendingWriteCount;
            for (entry = queue->PendingWrites.Flink;
                 entry != &queue->PendingWrites;
                 entry = entry->Flink) {
                PYUME_WRITE_REQUEST request;

                request = CONTAINING_RECORD(entry, YUME_WRITE_REQUEST, Link);
                DebugState->PendingWriteFragmentsIssued += request->NextIssueSeq;
                DebugState->PendingWriteFragmentsAcked += request->AckedCount;
            }
        }
        KeReleaseSpinLock(&queue->WriteQueueLock, oldIrql);
    }

    return STATUS_SUCCESS;
}
