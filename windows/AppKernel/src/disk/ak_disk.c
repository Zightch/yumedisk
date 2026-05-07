#include "disk/ak_disk.h"

#include "event/ak_event.h"
#include "protocol/ak_protocol.h"
#include "session/ak_session.h"

#include <string.h>

#define AK_DISK_WORKER_KIND_READ 1u
#define AK_DISK_WORKER_KIND_WRITE 2u
#define AK_DISK_WORKER_KIND_ACK 3u

#define AK_DISK_SLOT_ENGINE_POLL_MS 10u
#define AK_DISK_RECOVERABLE_RETRY_DELAY_MS 10u

typedef struct AK_READ_SLOT_CONTEXT {
    AK_PROTOCOL_ASYNC_IO AsyncIo;
    AK_PROTOCOL_MESSAGE_BUFFER RequestBuffer;
    YUMEDISK_READ_SLOT_EVENT Event;
    BYTE* ReadDataBuffer;
    UINT32 ReadDataCapacity;
    UINT64 SlotId;
    AK_STATUS IoStatus;
    UINT32 DataLength;
    BOOLEAN Active;
    BOOLEAN AckPending;
    DWORD RetryTick;
} AK_READ_SLOT_CONTEXT;

typedef struct AK_READ_ACK_CONTEXT {
    AK_PROTOCOL_ASYNC_IO AsyncIo;
    AK_PROTOCOL_MESSAGE_BUFFER RequestBuffer;
    UINT64 EventId;
    AK_STATUS IoStatus;
    UINT32 DataLength;
    BYTE* DataBuffer;
    BOOLEAN Active;
    BOOLEAN HasPayload;
    DWORD RetryTick;
} AK_READ_ACK_CONTEXT;

typedef struct AK_WRITE_SLOT_CONTEXT {
    AK_PROTOCOL_ASYNC_IO AsyncIo;
    AK_PROTOCOL_MESSAGE_BUFFER RequestBuffer;
    BYTE* SlotBuffer;
    UINT32 SlotBufferCapacity;
    UINT64 SlotId;
    DWORD RetryTick;
    BOOLEAN Active;
} AK_WRITE_SLOT_CONTEXT;

typedef struct AK_WRITE_ACK_FLUSH_CONTEXT {
    AK_PROTOCOL_ASYNC_IO AsyncIo;
    AK_PROTOCOL_MESSAGE_BUFFER RequestBuffer;
    YUMEDISK_WRITE_ACK_RANGE* Ranges;
    UINT32 RangeCount;
    UINT32 RangeCapacity;
    DWORD RetryTick;
    BOOLEAN Active;
    BOOLEAN HasPayload;
} AK_WRITE_ACK_FLUSH_CONTEXT;

typedef struct AK_WRITE_ACK_NODE {
    YUMEDISK_WRITE_ACK_RANGE Range;
    struct AK_WRITE_ACK_NODE* Next;
} AK_WRITE_ACK_NODE;

typedef struct AK_WRITE_EVENT_RECORD {
    UINT64 EventId;
    UINT32 TotalSeq;
    UINT32 AckedSeqCount;
    AK_STATUS FinalStatus;
    BOOLEAN FinalFailed;
    BOOLEAN FinalEventQueued;
    struct AK_WRITE_EVENT_RECORD* Next;
} AK_WRITE_EVENT_RECORD;

static void AkDiskFreeWorkerArrays(
    AK_DISK* disk);

static void AkDiskStopWorkers(
    AK_DISK* disk);

static void AkDiskRecordFinalWriteCommitted(
    AK_DISK* disk);

static void AkDiskRecordFinalWriteRejected(
    AK_DISK* disk);

static DWORD AkDiskGetRetryTick(void)
{
    return GetTickCount() + AK_DISK_RECOVERABLE_RETRY_DELAY_MS;
}

static BOOLEAN AkDiskIsRetryReady(
    DWORD retry_tick)
{
    DWORD now;
    LONG delta;

    if (retry_tick == 0u) {
        return TRUE;
    }

    now = GetTickCount();
    delta = (LONG)(now - retry_tick);
    return (BOOLEAN)(delta >= 0);
}

static BOOLEAN AkDiskIsStopRequested(
    const AK_DISK* disk)
{
    DWORD wait_status;

    if ((disk == NULL) || (disk->StopEvent == NULL)) {
        return TRUE;
    }

    wait_status = WaitForSingleObject(disk->StopEvent, 0u);
    return (BOOLEAN)(wait_status == WAIT_OBJECT_0);
}

static BOOLEAN AkDiskIsRecoverableSlotStatus(
    AK_STATUS status)
{
    return (BOOLEAN)((status == AK_STATUS_DEVICE_NOT_READY) ||
                     (status == AK_STATUS_CANCELLED));
}

static BOOLEAN AkDiskIsRecoverableReadAckStatus(
    AK_STATUS status)
{
    return (BOOLEAN)(AkDiskIsRecoverableSlotStatus(status) ||
                     (status == AK_STATUS_NOT_FOUND));
}

static BOOLEAN AkDiskIsRecoverableWriteAckStatus(
    AK_STATUS status)
{
    return (BOOLEAN)(AkDiskIsRecoverableSlotStatus(status) ||
                     (status == AK_STATUS_NOT_FOUND));
}

static UINT32 AkDiskComputeWriteWorkerSlotCount(
    const AK_DISK* disk,
    UINT32 worker_index)
{
    UINT32 worker_count;
    UINT32 base_count;
    UINT32 remainder;

    worker_count = (UINT32)disk->Params.WriteWorkerCount;
    if (worker_count == 0u) {
        return 0u;
    }

    base_count = disk->Params.QueueDepth / worker_count;
    remainder = disk->Params.QueueDepth % worker_count;
    if (worker_index < remainder) {
        base_count += 1u;
    }

    return base_count;
}

static AK_STATUS AkDiskEmitWriteFinalEvent(
    AK_DISK* disk,
    UINT64 event_id,
    UINT32 total_seq,
    AK_STATUS final_status,
    BOOLEAN committed)
{
    AK_EVENT event_record;
    AK_STATUS status;

    (void)memset(&event_record, 0, sizeof(event_record));
    event_record.Type = committed ? AkEventWriteFinalCommitted : AkEventWriteFinalRejected;
    event_record.TargetId = disk->State.TargetId;
    event_record.DiskRuntimeId = disk->State.DiskRuntimeId;
    event_record.EventId = event_id;
    event_record.TotalSeq = total_seq;
    event_record.Status = final_status;

    status = AkEventQueuePush(disk->Session, &event_record);
    if (status != AK_STATUS_SUCCESS) {
        return status;
    }

    if (committed) {
        AkDiskRecordFinalWriteCommitted(disk);
    } else {
        AkDiskRecordFinalWriteRejected(disk);
    }

    return AK_STATUS_SUCCESS;
}

static AK_WRITE_EVENT_RECORD* AkDiskFindActiveWriteEventLocked(
    AK_DISK* disk,
    UINT64 event_id)
{
    AK_WRITE_EVENT_RECORD* record;

    record = disk->ActiveWriteEvents;
    while (record != NULL) {
        if (record->EventId == event_id) {
            return record;
        }

        record = record->Next;
    }

    return NULL;
}

static AK_WRITE_EVENT_RECORD* AkDiskFindFinalizedWriteEventLocked(
    AK_DISK* disk,
    UINT64 event_id)
{
    AK_WRITE_EVENT_RECORD* record;

    record = disk->FinalizedWriteEventsHead;
    while (record != NULL) {
        if (record->EventId == event_id) {
            return record;
        }

        record = record->Next;
    }

    return NULL;
}

static AK_STATUS AkDiskRememberFinalizedWriteEventLocked(
    AK_DISK* disk,
    UINT64 event_id,
    UINT32 total_seq,
    AK_STATUS final_status,
    BOOLEAN final_failed)
{
    AK_WRITE_EVENT_RECORD* record;
    AK_WRITE_EVENT_RECORD* old_record;

    record = AkDiskFindFinalizedWriteEventLocked(disk, event_id);
    if (record != NULL) {
        record->TotalSeq = total_seq;
        record->FinalStatus = final_status;
        record->FinalFailed = final_failed;
        record->FinalEventQueued = TRUE;
        return AK_STATUS_SUCCESS;
    }

    record = (AK_WRITE_EVENT_RECORD*)AkAllocZero(sizeof(*record));
    if (record == NULL) {
        return AK_STATUS_INSUFFICIENT_RESOURCES;
    }

    record->EventId = event_id;
    record->TotalSeq = total_seq;
    record->AckedSeqCount = total_seq;
    record->FinalStatus = final_status;
    record->FinalFailed = final_failed;
    record->FinalEventQueued = TRUE;
    record->Next = NULL;

    if (disk->FinalizedWriteEventsTail != NULL) {
        disk->FinalizedWriteEventsTail->Next = record;
    } else {
        disk->FinalizedWriteEventsHead = record;
    }
    disk->FinalizedWriteEventsTail = record;
    disk->FinalizedWriteEventCount += 1u;

    while (disk->FinalizedWriteEventCount > disk->Params.QueueDepth) {
        old_record = disk->FinalizedWriteEventsHead;
        if (old_record == NULL) {
            break;
        }

        disk->FinalizedWriteEventsHead = old_record->Next;
        if (disk->FinalizedWriteEventsHead == NULL) {
            disk->FinalizedWriteEventsTail = NULL;
        }
        disk->FinalizedWriteEventCount -= 1u;
        AkFree(old_record);
    }

    return AK_STATUS_SUCCESS;
}

static AK_STATUS AkDiskFinalizeWriteEventLocked(
    AK_DISK* disk,
    AK_WRITE_EVENT_RECORD* record,
    AK_STATUS final_status,
    BOOLEAN final_failed)
{
    AK_STATUS status;

    if ((disk == NULL) || (record == NULL)) {
        return AK_STATUS_INVALID_PARAMETER;
    }

    if (record->FinalEventQueued) {
        return AK_STATUS_SUCCESS;
    }

    record->FinalFailed = final_failed;
    record->FinalStatus = final_status;
    status = AkDiskEmitWriteFinalEvent(
        disk,
        record->EventId,
        record->TotalSeq,
        final_status,
        (BOOLEAN)!final_failed);
    if (status != AK_STATUS_SUCCESS) {
        return status;
    }

    record->FinalEventQueued = TRUE;
    return AK_STATUS_SUCCESS;
}

static AK_STATUS AkDiskEnsureWriteEventTracked(
    AK_DISK* disk,
    UINT64 event_id,
    UINT32 total_seq,
    AK_STATUS* out_existing_final_status)
{
    AK_WRITE_EVENT_RECORD* record;
    AK_STATUS status;

    if ((disk == NULL) || (event_id == 0ull) || (total_seq == 0u)) {
        return AK_STATUS_INVALID_PARAMETER;
    }

    if (out_existing_final_status != NULL) {
        *out_existing_final_status = AK_STATUS_SUCCESS;
    }

    AcquireSRWLockExclusive(&disk->WriteTrackLock);

    record = AkDiskFindFinalizedWriteEventLocked(disk, event_id);
    if (record != NULL) {
        if (out_existing_final_status != NULL) {
            *out_existing_final_status = record->FinalStatus;
        }
        ReleaseSRWLockExclusive(&disk->WriteTrackLock);
        return AK_STATUS_ALREADY_EXISTS;
    }

    record = AkDiskFindActiveWriteEventLocked(disk, event_id);
    if (record != NULL) {
        if (record->TotalSeq != total_seq) {
            ReleaseSRWLockExclusive(&disk->WriteTrackLock);
            return AK_STATUS_INVALID_PARAMETER;
        }

        ReleaseSRWLockExclusive(&disk->WriteTrackLock);
        return AK_STATUS_SUCCESS;
    }

    record = (AK_WRITE_EVENT_RECORD*)AkAllocZero(sizeof(*record));
    if (record == NULL) {
        ReleaseSRWLockExclusive(&disk->WriteTrackLock);
        return AK_STATUS_INSUFFICIENT_RESOURCES;
    }

    record->EventId = event_id;
    record->TotalSeq = total_seq;
    record->AckedSeqCount = 0u;
    record->FinalStatus = AK_STATUS_SUCCESS;
    record->FinalFailed = FALSE;
    record->FinalEventQueued = FALSE;
    record->Next = disk->ActiveWriteEvents;
    disk->ActiveWriteEvents = record;

    ReleaseSRWLockExclusive(&disk->WriteTrackLock);
    status = AK_STATUS_SUCCESS;
    return status;
}

static void AkDiskFreeWriteEventList(
    AK_WRITE_EVENT_RECORD* head)
{
    AK_WRITE_EVENT_RECORD* record;
    AK_WRITE_EVENT_RECORD* next_record;

    record = head;
    while (record != NULL) {
        next_record = record->Next;
        AkFree(record);
        record = next_record;
    }
}

static void AkDiskFreeWriteAckList(
    AK_WRITE_ACK_NODE* head)
{
    AK_WRITE_ACK_NODE* node;
    AK_WRITE_ACK_NODE* next_node;

    node = head;
    while (node != NULL) {
        next_node = node->Next;
        AkFree(node);
        node = next_node;
    }
}

static AK_STATUS AkDiskEnqueueWriteAckRange(
    AK_DISK* disk,
    const YUMEDISK_WRITE_ACK_RANGE* range)
{
    AK_WRITE_ACK_NODE* node;

    if ((disk == NULL) || (range == NULL)) {
        return AK_STATUS_INVALID_PARAMETER;
    }

    node = (AK_WRITE_ACK_NODE*)AkAllocZero(sizeof(*node));
    if (node == NULL) {
        return AK_STATUS_INSUFFICIENT_RESOURCES;
    }

    node->Range = *range;
    node->Next = NULL;

    AcquireSRWLockExclusive(&disk->WriteAckLock);
    if (disk->WriteAckTail != NULL) {
        disk->WriteAckTail->Next = node;
    } else {
        disk->WriteAckHead = node;
    }
    disk->WriteAckTail = node;
    disk->PendingWriteAckCount += 1u;
    ReleaseSRWLockExclusive(&disk->WriteAckLock);

    if (disk->WriteAckWakeEvent != NULL) {
        SetEvent(disk->WriteAckWakeEvent);
    }

    return AK_STATUS_SUCCESS;
}

static UINT32 AkDiskStealWriteAckRanges(
    AK_DISK* disk,
    YUMEDISK_WRITE_ACK_RANGE* ranges,
    UINT32 max_range_count)
{
    UINT32 count;

    if ((disk == NULL) || (ranges == NULL) || (max_range_count == 0u)) {
        return 0u;
    }

    count = 0u;

    AcquireSRWLockExclusive(&disk->WriteAckLock);
    while ((disk->WriteAckHead != NULL) && (count < max_range_count)) {
        AK_WRITE_ACK_NODE* node;

        node = disk->WriteAckHead;
        disk->WriteAckHead = node->Next;
        if (disk->WriteAckHead == NULL) {
            disk->WriteAckTail = NULL;
        }
        if (disk->PendingWriteAckCount > 0u) {
            disk->PendingWriteAckCount -= 1u;
        }

        ranges[count] = node->Range;
        count += 1u;
        AkFree(node);
    }

    if (disk->WriteAckHead == NULL && disk->WriteAckWakeEvent != NULL) {
        ResetEvent(disk->WriteAckWakeEvent);
    }
    ReleaseSRWLockExclusive(&disk->WriteAckLock);

    return count;
}

static UINT32 AkDiskQueryPendingWriteAckCount(
    AK_DISK* disk)
{
    UINT32 count;

    if (disk == NULL) {
        return 0u;
    }

    AcquireSRWLockShared(&disk->WriteAckLock);
    count = disk->PendingWriteAckCount;
    ReleaseSRWLockShared(&disk->WriteAckLock);
    return count;
}

static void AkDiskClearPendingWriteAcks(
    AK_DISK* disk)
{
    AK_WRITE_ACK_NODE* head;

    if (disk == NULL) {
        return;
    }

    AcquireSRWLockExclusive(&disk->WriteAckLock);
    head = disk->WriteAckHead;
    disk->WriteAckHead = NULL;
    disk->WriteAckTail = NULL;
    disk->PendingWriteAckCount = 0u;
    if (disk->WriteAckWakeEvent != NULL) {
        ResetEvent(disk->WriteAckWakeEvent);
    }
    ReleaseSRWLockExclusive(&disk->WriteAckLock);

    AkDiskFreeWriteAckList(head);
}

static AK_STATUS AkDiskValidateWriteSlotHeader(
    AK_DISK* disk,
    const YUMEDISK_WRITE_SLOT_HEADER* header,
    UINT32 slot_capacity)
{
    if ((header == NULL) || (header->EventId == 0ull) ||
        (header->TargetId != disk->Params.TargetId) ||
        (header->TotalSeq == 0u) ||
        (header->Seq >= header->TotalSeq) ||
        (header->DataLength > (slot_capacity - YUMEDISK_WRITE_SLOT_HEADER_BASE_SIZE)) ||
        ((header->ByteOffsetInWrite % disk->Params.SectorSize) != 0u) ||
        ((header->DataLength != 0u) &&
         ((header->DataLength % disk->Params.SectorSize) != 0u))) {
        return AK_STATUS_INVALID_PARAMETER;
    }

    return AK_STATUS_SUCCESS;
}

static AK_STATUS AkDiskStageWriteSlot(
    AK_DISK* disk,
    const YUMEDISK_WRITE_SLOT_HEADER* header)
{
    AK_WRITE_OP write_op;
    AK_STATUS status;
    AK_STATUS existing_final_status;

    if ((disk == NULL) || (header == NULL)) {
        return AK_STATUS_INVALID_PARAMETER;
    }

    status = AkDiskEnsureWriteEventTracked(
        disk,
        header->EventId,
        header->TotalSeq,
        &existing_final_status);
    if (status == AK_STATUS_ALREADY_EXISTS) {
        return existing_final_status == AK_STATUS_SUCCESS ?
            AK_STATUS_CANCELLED : existing_final_status;
    }
    if (status != AK_STATUS_SUCCESS) {
        return status;
    }

    (void)memset(&write_op, 0, sizeof(write_op));
    write_op.TargetId = disk->Params.TargetId;
    write_op.DiskRuntimeId = disk->State.DiskRuntimeId;
    write_op.EventId = header->EventId;
    write_op.Seq = header->Seq;
    write_op.TotalSeq = header->TotalSeq;
    write_op.Lba = header->Lba;
    write_op.OffsetBytes = header->Lba * (UINT64)disk->Params.SectorSize;
    write_op.ByteOffsetInWrite = header->ByteOffsetInWrite;
    write_op.DataLength = header->DataLength;
    write_op.Flags = header->Flags;

    status = disk->MediaOps.stage_write(
        disk->MediaCtx,
        &write_op,
        header->DataLength == 0u ? NULL : header->Data,
        header->DataLength);
    return status;
}

static AK_STATUS AkDiskInitializeWriteSlotContext(
    AK_WRITE_SLOT_CONTEXT* slot_context,
    UINT32 slot_capacity)
{
    AK_STATUS status;

    (void)memset(slot_context, 0, sizeof(*slot_context));

    status = AkProtocolMessageAllocate(
        YumeDiskCommandPostWriteSlot,
        0u,
        0u,
        &slot_context->RequestBuffer);
    if (status != AK_STATUS_SUCCESS) {
        return status;
    }

    status = AkProtocolAsyncIoInitialize(&slot_context->AsyncIo);
    if (status != AK_STATUS_SUCCESS) {
        AkProtocolMessageRelease(&slot_context->RequestBuffer);
        return status;
    }

    slot_context->SlotBuffer = (BYTE*)AkAllocZero(slot_capacity);
    if (slot_context->SlotBuffer == NULL) {
        AkProtocolAsyncIoDestroy(&slot_context->AsyncIo);
        AkProtocolMessageRelease(&slot_context->RequestBuffer);
        return AK_STATUS_INSUFFICIENT_RESOURCES;
    }

    slot_context->SlotBufferCapacity = slot_capacity;
    return AK_STATUS_SUCCESS;
}

static void AkDiskDestroyWriteSlotContext(
    AK_WRITE_SLOT_CONTEXT* slot_context)
{
    if (slot_context == NULL) {
        return;
    }

    AkFree(slot_context->SlotBuffer);
    slot_context->SlotBuffer = NULL;
    slot_context->SlotBufferCapacity = 0u;
    AkProtocolAsyncIoDestroy(&slot_context->AsyncIo);
    AkProtocolMessageRelease(&slot_context->RequestBuffer);
}

static AK_STATUS AkDiskInitializeWriteAckFlushContext(
    AK_WRITE_ACK_FLUSH_CONTEXT* ack_context,
    UINT32 max_ranges)
{
    AK_STATUS status;
    ULONG payload_capacity;

    (void)memset(ack_context, 0, sizeof(*ack_context));

    payload_capacity = AkProtocolWriteAckBatchPayloadSize(max_ranges);
    status = AkProtocolMessageAllocate(
        YumeDiskCommandWriteAckBatch,
        0u,
        payload_capacity,
        &ack_context->RequestBuffer);
    if (status != AK_STATUS_SUCCESS) {
        return status;
    }

    status = AkProtocolAsyncIoInitialize(&ack_context->AsyncIo);
    if (status != AK_STATUS_SUCCESS) {
        AkProtocolMessageRelease(&ack_context->RequestBuffer);
        return status;
    }

    ack_context->Ranges = (YUMEDISK_WRITE_ACK_RANGE*)AkAllocZero(
        (size_t)max_ranges * sizeof(YUMEDISK_WRITE_ACK_RANGE));
    if (ack_context->Ranges == NULL) {
        AkProtocolAsyncIoDestroy(&ack_context->AsyncIo);
        AkProtocolMessageRelease(&ack_context->RequestBuffer);
        return AK_STATUS_INSUFFICIENT_RESOURCES;
    }

    ack_context->RangeCapacity = max_ranges;
    return AK_STATUS_SUCCESS;
}

static void AkDiskResetWriteAckFlushPayload(
    AK_WRITE_ACK_FLUSH_CONTEXT* ack_context)
{
    if (ack_context == NULL) {
        return;
    }

    ack_context->RangeCount = 0u;
    ack_context->HasPayload = FALSE;
    ack_context->RetryTick = 0u;
}

static void AkDiskDestroyWriteAckFlushContext(
    AK_WRITE_ACK_FLUSH_CONTEXT* ack_context)
{
    if (ack_context == NULL) {
        return;
    }

    AkFree(ack_context->Ranges);
    ack_context->Ranges = NULL;
    ack_context->RangeCapacity = 0u;
    AkProtocolAsyncIoDestroy(&ack_context->AsyncIo);
    AkProtocolMessageRelease(&ack_context->RequestBuffer);
}

static void AkDiskRecordReadSlotPost(
    AK_DISK* disk)
{
    AcquireSRWLockExclusive(&disk->Lock);
    disk->Stats.ReadSlotPosts += 1ull;
    ReleaseSRWLockExclusive(&disk->Lock);
}

static void AkDiskRecordReadSlotCompletion(
    AK_DISK* disk)
{
    AcquireSRWLockExclusive(&disk->Lock);
    disk->Stats.ReadSlotCompletions += 1ull;
    ReleaseSRWLockExclusive(&disk->Lock);
}

static void AkDiskRecordReadAckCommand(
    AK_DISK* disk)
{
    AcquireSRWLockExclusive(&disk->Lock);
    disk->Stats.ReadAckCommands += 1ull;
    ReleaseSRWLockExclusive(&disk->Lock);
}

static void AkDiskRecordWriteSlotPost(
    AK_DISK* disk)
{
    AcquireSRWLockExclusive(&disk->Lock);
    disk->Stats.WriteSlotPosts += 1ull;
    ReleaseSRWLockExclusive(&disk->Lock);
}

static void AkDiskRecordWriteSlotCompletion(
    AK_DISK* disk)
{
    AcquireSRWLockExclusive(&disk->Lock);
    disk->Stats.WriteSlotCompletions += 1ull;
    ReleaseSRWLockExclusive(&disk->Lock);
}

static void AkDiskRecordWriteAckFlush(
    AK_DISK* disk,
    UINT32 range_count)
{
    AcquireSRWLockExclusive(&disk->Lock);
    disk->Stats.WriteAckFlushes += 1ull;
    disk->Stats.WriteAckRanges += (UINT64)range_count;
    ReleaseSRWLockExclusive(&disk->Lock);
}

static void AkDiskRecordWriteAckRangeFailures(
    AK_DISK* disk,
    UINT32 failure_count)
{
    AcquireSRWLockExclusive(&disk->Lock);
    disk->Stats.WriteAckRangeFailures += (UINT64)failure_count;
    ReleaseSRWLockExclusive(&disk->Lock);
}

static void AkDiskRecordFinalWriteCommitted(
    AK_DISK* disk)
{
    AcquireSRWLockExclusive(&disk->Lock);
    disk->Stats.FinalWriteCommitted += 1ull;
    ReleaseSRWLockExclusive(&disk->Lock);
}

static void AkDiskRecordFinalWriteRejected(
    AK_DISK* disk)
{
    AcquireSRWLockExclusive(&disk->Lock);
    disk->Stats.FinalWriteRejected += 1ull;
    ReleaseSRWLockExclusive(&disk->Lock);
}

static void AkDiskRecordCommandFailure(
    AK_DISK* disk,
    AK_STATUS status)
{
    AcquireSRWLockExclusive(&disk->Session->Lock);
    disk->Session->Stats.CommandFailures += 1ull;
    disk->Session->State.LastError = status;
    ReleaseSRWLockExclusive(&disk->Session->Lock);

    AcquireSRWLockExclusive(&disk->Lock);
    disk->State.LastError = status;
    ReleaseSRWLockExclusive(&disk->Lock);
}

static void AkDiskRecordProtocolFailure(
    AK_DISK* disk,
    AK_STATUS status)
{
    AcquireSRWLockExclusive(&disk->Session->Lock);
    disk->Session->Stats.ProtocolFailures += 1ull;
    disk->Session->State.LastError = status;
    ReleaseSRWLockExclusive(&disk->Session->Lock);

    AcquireSRWLockExclusive(&disk->Lock);
    disk->State.LastError = status;
    ReleaseSRWLockExclusive(&disk->Lock);
}

static void AkDiskSetBroken(
    AK_DISK* disk,
    AK_STATUS status)
{
    AcquireSRWLockExclusive(&disk->Lock);
    disk->State.Lifecycle = AkStateBroken;
    disk->State.LastError = status;
    ReleaseSRWLockExclusive(&disk->Lock);
}

static AK_STATUS AkDiskValidateParams(
    const AK_DISK_PARAMS* params,
    const AK_MEDIA_OPS* media_ops)
{
    if ((params == NULL) || (media_ops == NULL)) {
        return AK_STATUS_INVALID_PARAMETER;
    }

    if ((params->SectorSize == 0u) || (params->DiskSizeBytes == 0ull) ||
        ((params->DiskSizeBytes % params->SectorSize) != 0ull)) {
        return AK_STATUS_INVALID_PARAMETER;
    }

    if (params->QueueDepth == 0u) {
        return AK_STATUS_INVALID_PARAMETER;
    }

    if (params->WriteSlotBytes == 0u) {
        return AK_STATUS_INVALID_PARAMETER;
    }

    if ((params->ReadWorkerCount == 0u) || (params->WriteWorkerCount == 0u)) {
        return AK_STATUS_INVALID_PARAMETER;
    }

    if (params->AckBatchMaxRanges == 0u) {
        return AK_STATUS_INVALID_PARAMETER;
    }

    if (params->TargetId > YUMEDISK_MAX_USABLE_TARGET_ID) {
        return AK_STATUS_INVALID_PARAMETER;
    }

    if ((media_ops->read_bytes == NULL) || (media_ops->stage_write == NULL)) {
        return AK_STATUS_INVALID_PARAMETER;
    }

    return AK_STATUS_SUCCESS;
}

static void AkDiskInitializeState(
    AK_DISK* disk)
{
    disk->State.Lifecycle = AkStateInit;
    disk->State.TargetId = disk->Params.TargetId;
    disk->State.DiskRuntimeId = 0ull;
    disk->State.ReadWorkersRunning = FALSE;
    disk->State.WriteWorkersRunning = FALSE;
    disk->State.AckFlusherRunning = FALSE;
    disk->State.LastError = AK_STATUS_SUCCESS;
}

static void AkDiskSetLastError(
    AK_DISK* disk,
    AK_STATUS status)
{
    AcquireSRWLockExclusive(&disk->Lock);
    disk->State.LastError = status;
    ReleaseSRWLockExclusive(&disk->Lock);
}

static UINT32 AkDiskComputeReadWorkerSlotCount(
    const AK_DISK* disk,
    UINT32 worker_index)
{
    UINT32 worker_count;
    UINT32 base_count;
    UINT32 remainder;

    worker_count = (UINT32)disk->Params.ReadWorkerCount;
    if (worker_count == 0u) {
        return 0u;
    }

    base_count = disk->Params.QueueDepth / worker_count;
    remainder = disk->Params.QueueDepth % worker_count;
    if (worker_index < remainder) {
        base_count += 1u;
    }

    return base_count;
}

static AK_STATUS AkDiskAllocateWorkerArrays(
    AK_DISK* disk)
{
    size_t read_count;
    size_t write_count;

    read_count = (size_t)disk->Params.ReadWorkerCount;
    write_count = (size_t)disk->Params.WriteWorkerCount;

    disk->ReadWorkerThreads = (HANDLE*)AkAllocZero(read_count * sizeof(HANDLE));
    disk->ReadWorkerContexts = (AK_DISK_WORKER_CONTEXT*)AkAllocZero(
        read_count * sizeof(AK_DISK_WORKER_CONTEXT));
    disk->WriteWorkerThreads = (HANDLE*)AkAllocZero(write_count * sizeof(HANDLE));
    disk->WriteWorkerContexts = (AK_DISK_WORKER_CONTEXT*)AkAllocZero(
        write_count * sizeof(AK_DISK_WORKER_CONTEXT));

    if ((disk->ReadWorkerThreads == NULL) || (disk->ReadWorkerContexts == NULL) ||
        (disk->WriteWorkerThreads == NULL) || (disk->WriteWorkerContexts == NULL)) {
        AkDiskFreeWorkerArrays(disk);
        return AK_STATUS_INSUFFICIENT_RESOURCES;
    }

    return AK_STATUS_SUCCESS;
}

static void AkDiskFreeWorkerArrays(
    AK_DISK* disk)
{
    AkFree(disk->ReadWorkerThreads);
    AkFree(disk->ReadWorkerContexts);
    AkFree(disk->WriteWorkerThreads);
    AkFree(disk->WriteWorkerContexts);

    disk->ReadWorkerThreads = NULL;
    disk->ReadWorkerContexts = NULL;
    disk->WriteWorkerThreads = NULL;
    disk->WriteWorkerContexts = NULL;
}

static AK_STATUS AkDiskValidateReadSlotEvent(
    AK_DISK* disk,
    const YUMEDISK_READ_SLOT_EVENT* event_record)
{
    UINT64 bytes_from_blocks;

    if ((event_record == NULL) || (event_record->EventId == 0ull)) {
        return AK_STATUS_UNSUCCESSFUL;
    }

    if (event_record->TargetId != disk->Params.TargetId) {
        return AK_STATUS_UNSUCCESSFUL;
    }

    if ((event_record->DataLength != 0u) &&
        ((event_record->DataLength % disk->Params.SectorSize) != 0u)) {
        return AK_STATUS_UNSUCCESSFUL;
    }

    bytes_from_blocks =
        (UINT64)event_record->BlockCount * (UINT64)disk->Params.SectorSize;
    if ((event_record->BlockCount != 0u) &&
        (bytes_from_blocks != (UINT64)event_record->DataLength)) {
        return AK_STATUS_UNSUCCESSFUL;
    }

    return AK_STATUS_SUCCESS;
}

static void AkDiskResetReadSlotPendingState(
    AK_READ_SLOT_CONTEXT* slot_context)
{
    slot_context->AckPending = FALSE;
    (void)memset(&slot_context->Event, 0, sizeof(slot_context->Event));
    slot_context->IoStatus = AK_STATUS_SUCCESS;
    slot_context->DataLength = 0u;
}

static void AkDiskResetReadAckPayload(
    AK_READ_ACK_CONTEXT* ack_context)
{
    ack_context->HasPayload = FALSE;
    ack_context->EventId = 0ull;
    ack_context->IoStatus = AK_STATUS_SUCCESS;
    ack_context->DataLength = 0u;
    ack_context->DataBuffer = NULL;
    ack_context->RetryTick = 0u;
}

static AK_STATUS AkDiskEnsureReadBufferCapacity(
    AK_READ_SLOT_CONTEXT* slot_context,
    UINT32 required_capacity)
{
    BYTE* new_buffer;

    if (required_capacity <= slot_context->ReadDataCapacity) {
        return AK_STATUS_SUCCESS;
    }

    new_buffer = (BYTE*)AkAllocZero(required_capacity);
    if (new_buffer == NULL) {
        return AK_STATUS_INSUFFICIENT_RESOURCES;
    }

    AkFree(slot_context->ReadDataBuffer);
    slot_context->ReadDataBuffer = new_buffer;
    slot_context->ReadDataCapacity = required_capacity;
    return AK_STATUS_SUCCESS;
}

static AK_STATUS AkDiskCopyReadData(
    AK_DISK* disk,
    const YUMEDISK_READ_SLOT_EVENT* event_record,
    BYTE* buffer,
    UINT32 buffer_capacity,
    UINT32* out_data_length)
{
    AK_READ_OP read_op;

    if ((event_record == NULL) || (buffer == NULL) || (out_data_length == NULL)) {
        return AK_STATUS_INVALID_PARAMETER;
    }

    (void)memset(&read_op, 0, sizeof(read_op));
    read_op.TargetId = disk->Params.TargetId;
    read_op.DiskRuntimeId = disk->State.DiskRuntimeId;
    read_op.EventId = event_record->EventId;
    read_op.Lba = event_record->Lba;
    read_op.OffsetBytes = event_record->Lba * (UINT64)disk->Params.SectorSize;
    read_op.BlockCount = event_record->BlockCount;
    read_op.DataLength = event_record->DataLength;
    read_op.Flags = 0u;

    if (event_record->DataLength > buffer_capacity) {
        *out_data_length = 0u;
        return AK_STATUS_INVALID_PARAMETER;
    }

    *out_data_length = event_record->DataLength;
    {
        AK_STATUS status;

        status = disk->MediaOps.read_bytes(
            disk->MediaCtx,
            &read_op,
            buffer,
            out_data_length);
        if ((status == AK_STATUS_SUCCESS) &&
            (*out_data_length != event_record->DataLength)) {
            *out_data_length = 0u;
            return AK_STATUS_UNSUCCESSFUL;
        }

        if (status != AK_STATUS_SUCCESS) {
            *out_data_length = 0u;
        }

        return status;
    }
}

static AK_STATUS AkDiskInitializeReadSlotContext(
    AK_READ_SLOT_CONTEXT* slot_context)
{
    AK_STATUS status;

    (void)memset(slot_context, 0, sizeof(*slot_context));

    status = AkProtocolMessageAllocate(
        YumeDiskCommandPostReadSlot,
        0u,
        0u,
        &slot_context->RequestBuffer);
    if (status != AK_STATUS_SUCCESS) {
        return status;
    }

    status = AkProtocolAsyncIoInitialize(&slot_context->AsyncIo);
    if (status != AK_STATUS_SUCCESS) {
        AkProtocolMessageRelease(&slot_context->RequestBuffer);
        return status;
    }

    return AK_STATUS_SUCCESS;
}

static void AkDiskDestroyReadSlotContext(
    AK_READ_SLOT_CONTEXT* slot_context)
{
    AkProtocolAsyncIoDestroy(&slot_context->AsyncIo);
    AkProtocolMessageRelease(&slot_context->RequestBuffer);
}

static AK_STATUS AkDiskInitializeReadAckContext(
    AK_READ_ACK_CONTEXT* ack_context)
{
    AK_STATUS status;

    (void)memset(ack_context, 0, sizeof(*ack_context));

    status = AkProtocolMessageAllocate(
        YumeDiskCommandReadAck,
        (ULONG)sizeof(YUMEDISK_READ_ACK),
        (ULONG)sizeof(YUMEDISK_READ_ACK),
        &ack_context->RequestBuffer);
    if (status != AK_STATUS_SUCCESS) {
        return status;
    }

    status = AkProtocolAsyncIoInitialize(&ack_context->AsyncIo);
    if (status != AK_STATUS_SUCCESS) {
        AkProtocolMessageRelease(&ack_context->RequestBuffer);
        return status;
    }

    return AK_STATUS_SUCCESS;
}

static void AkDiskDestroyReadAckContext(
    AK_READ_ACK_CONTEXT* ack_context)
{
    AkProtocolAsyncIoDestroy(&ack_context->AsyncIo);
    AkProtocolMessageRelease(&ack_context->RequestBuffer);
}

static AK_STATUS AkDiskPostReadSlotAsync(
    AK_DISK* disk,
    HANDLE control_file,
    UINT64 session_id,
    AK_READ_SLOT_CONTEXT* slot_context)
{
    AK_STATUS status;
    UINT64 slot_id;

    slot_id = AkSessionAllocateTxId(disk->Session);
    if (slot_id == 0ull) {
        return AK_STATUS_UNSUCCESSFUL;
    }

    slot_context->SlotId = slot_id;
    (void)memset(&slot_context->Event, 0, sizeof(slot_context->Event));

    status = AkProtocolPreparePostReadSlot(
        &slot_context->RequestBuffer,
        session_id,
        disk->Params.TargetId,
        slot_id);
    if (status != AK_STATUS_SUCCESS) {
        slot_context->SlotId = 0ull;
        return status;
    }

    AkDiskRecordReadSlotPost(disk);
    status = AkProtocolAsyncIoBegin(
        control_file,
        slot_context->RequestBuffer.Message,
        slot_context->RequestBuffer.Size,
        &slot_context->Event,
        (DWORD)sizeof(slot_context->Event),
        &slot_context->AsyncIo);
    if (status != AK_STATUS_SUCCESS) {
        slot_context->SlotId = 0ull;
        if (AkDiskIsRecoverableSlotStatus(status)) {
            slot_context->RetryTick = AkDiskGetRetryTick();
            return AK_STATUS_SUCCESS;
        }

        return status;
    }

    slot_context->Active = TRUE;
    slot_context->RetryTick = 0u;
    return AK_STATUS_SUCCESS;
}

static AK_STATUS AkDiskBeginReadAckAsync(
    AK_DISK* disk,
    HANDLE control_file,
    UINT64 session_id,
    AK_READ_ACK_CONTEXT* ack_context)
{
    AK_STATUS status;
    YUMEDISK_READ_ACK ack;

    ack.EventId = ack_context->EventId;
    ack.IoStatus = ack_context->IoStatus;
    ack.DataLength = ack_context->DataLength;
    ack.KernelVa = 0ull;

    status = AkProtocolPrepareReadAck(
        &ack_context->RequestBuffer,
        session_id,
        disk->Params.TargetId,
        &ack);
    if (status != AK_STATUS_SUCCESS) {
        return status;
    }

    status = AkProtocolAsyncIoBegin(
        control_file,
        ack_context->RequestBuffer.Message,
        ack_context->RequestBuffer.Size,
        ack_context->DataLength == 0u ? NULL : ack_context->DataBuffer,
        ack_context->DataLength,
        &ack_context->AsyncIo);
    if (status != AK_STATUS_SUCCESS) {
        if (AkDiskIsRecoverableReadAckStatus(status)) {
            ack_context->RetryTick = AkDiskGetRetryTick();
            return AK_STATUS_SUCCESS;
        }

        return status;
    }

    ack_context->Active = TRUE;
    ack_context->RetryTick = 0u;
    return AK_STATUS_SUCCESS;
}

static AK_STATUS AkDiskHandleReadSlotCompletion(
    AK_DISK* disk,
    HANDLE control_file,
    AK_READ_SLOT_CONTEXT* slot_context)
{
    AK_STATUS status;
    DWORD bytes_transferred;

    slot_context->Active = FALSE;
    slot_context->SlotId = 0ull;

    status = AkProtocolAsyncIoFinish(
        control_file,
        &slot_context->AsyncIo,
        &bytes_transferred);
    if (status != AK_STATUS_SUCCESS) {
        if (AkDiskIsStopRequested(disk)) {
            AkDiskResetReadSlotPendingState(slot_context);
            return AK_STATUS_SUCCESS;
        }

        if (AkDiskIsRecoverableSlotStatus(status)) {
            slot_context->RetryTick = AkDiskGetRetryTick();
            return AK_STATUS_SUCCESS;
        }

        return status;
    }

    (void)bytes_transferred;
    status = AkDiskValidateReadSlotEvent(disk, &slot_context->Event);
    if (status != AK_STATUS_SUCCESS) {
        AkDiskRecordProtocolFailure(disk, status);
        AkDiskResetReadSlotPendingState(slot_context);
        return AK_STATUS_SUCCESS;
    }

    status = AkDiskEnsureReadBufferCapacity(
        slot_context,
        slot_context->Event.DataLength);
    if (status != AK_STATUS_SUCCESS) {
        slot_context->IoStatus = status;
        slot_context->DataLength = 0u;
        slot_context->AckPending = TRUE;
        slot_context->RetryTick = 0u;
        AkDiskRecordReadSlotCompletion(disk);
        return AK_STATUS_SUCCESS;
    }

    status = AkDiskCopyReadData(
        disk,
        &slot_context->Event,
        slot_context->ReadDataBuffer,
        slot_context->ReadDataCapacity,
        &slot_context->DataLength);
    slot_context->IoStatus = status;
    slot_context->AckPending = TRUE;
    slot_context->RetryTick = 0u;

    AkDiskRecordReadSlotCompletion(disk);
    return AK_STATUS_SUCCESS;
}

static AK_STATUS AkDiskHandleReadAckCompletion(
    AK_DISK* disk,
    HANDLE control_file,
    AK_READ_ACK_CONTEXT* ack_context)
{
    AK_STATUS status;
    DWORD bytes_transferred;

    ack_context->Active = FALSE;

    status = AkProtocolAsyncIoFinish(
        control_file,
        &ack_context->AsyncIo,
        &bytes_transferred);
    if (status != AK_STATUS_SUCCESS) {
        if (AkDiskIsStopRequested(disk)) {
            AkDiskResetReadAckPayload(ack_context);
            return AK_STATUS_SUCCESS;
        }

        if (AkDiskIsRecoverableReadAckStatus(status)) {
            ack_context->RetryTick = AkDiskGetRetryTick();
            return AK_STATUS_SUCCESS;
        }

        return status;
    }

    (void)bytes_transferred;
    if (ack_context->RequestBuffer.Message->Header.Status != AK_STATUS_SUCCESS) {
        status = ack_context->RequestBuffer.Message->Header.Status;
        if (AkDiskIsRecoverableReadAckStatus(status)) {
            ack_context->RetryTick = AkDiskGetRetryTick();
            return AK_STATUS_SUCCESS;
        }

        AkDiskResetReadAckPayload(ack_context);
        return AK_STATUS_SUCCESS;
    }

    AkDiskRecordReadAckCommand(disk);
    AkDiskResetReadAckPayload(ack_context);
    return AK_STATUS_SUCCESS;
}

static AK_STATUS AkDiskDispatchPendingReadAcks(
    AK_DISK* disk,
    HANDLE control_file,
    UINT64 session_id,
    AK_READ_SLOT_CONTEXT* slot_contexts,
    AK_READ_ACK_CONTEXT* ack_contexts,
    UINT32 slot_count)
{
    UINT32 slot_index;

    for (slot_index = 0u; slot_index < slot_count; ++slot_index) {
        AK_READ_SLOT_CONTEXT* slot_context;
        UINT32 ack_index;
        BOOLEAN found;

        slot_context = &slot_contexts[slot_index];
        if (!slot_context->AckPending) {
            continue;
        }

        found = FALSE;
        for (ack_index = 0u; ack_index < slot_count; ++ack_index) {
            AK_READ_ACK_CONTEXT* ack_context;

            ack_context = &ack_contexts[ack_index];
            if (ack_context->HasPayload || ack_context->Active) {
                continue;
            }

            ack_context->EventId = slot_context->Event.EventId;
            ack_context->IoStatus = slot_context->IoStatus;
            ack_context->DataLength = slot_context->DataLength;
            ack_context->DataBuffer = slot_context->ReadDataBuffer;
            ack_context->HasPayload = TRUE;
            ack_context->RetryTick = 0u;

            AkDiskResetReadSlotPendingState(slot_context);
            found = TRUE;
            break;
        }

        if (!found) {
            return AK_STATUS_SUCCESS;
        }
    }

    for (slot_index = 0u; slot_index < slot_count; ++slot_index) {
        AK_READ_ACK_CONTEXT* ack_context;

        ack_context = &ack_contexts[slot_index];
        if (!ack_context->HasPayload || ack_context->Active ||
            !AkDiskIsRetryReady(ack_context->RetryTick)) {
            continue;
        }

        {
            AK_STATUS ack_status;

            ack_status = AkDiskBeginReadAckAsync(disk, control_file, session_id, ack_context);
            if (ack_status != AK_STATUS_SUCCESS) {
                return ack_status;
            }
        }
    }

    return AK_STATUS_SUCCESS;
}

static AK_STATUS AkDiskDrainCompletedReadPlane(
    AK_DISK* disk,
    HANDLE control_file,
    AK_READ_SLOT_CONTEXT* slot_contexts,
    AK_READ_ACK_CONTEXT* ack_contexts,
    UINT32 slot_count)
{
    UINT32 index;
    BOOLEAN progressed;

    do {
        progressed = FALSE;

        for (index = 0u; index < slot_count; ++index) {
            AK_READ_ACK_CONTEXT* ack_context;
            AK_STATUS completion_status;

            ack_context = &ack_contexts[index];
            if (!ack_context->Active) {
                continue;
            }

            if (WaitForSingleObject(ack_context->AsyncIo.Overlapped.hEvent, 0u) != WAIT_OBJECT_0) {
                continue;
            }

            completion_status = AkDiskHandleReadAckCompletion(disk, control_file, ack_context);
            if (completion_status != AK_STATUS_SUCCESS) {
                return completion_status;
            }

            progressed = TRUE;
        }

        for (index = 0u; index < slot_count; ++index) {
            AK_READ_SLOT_CONTEXT* slot_context;
            AK_STATUS completion_status;

            slot_context = &slot_contexts[index];
            if (!slot_context->Active) {
                continue;
            }

            if (WaitForSingleObject(slot_context->AsyncIo.Overlapped.hEvent, 0u) != WAIT_OBJECT_0) {
                continue;
            }

            completion_status = AkDiskHandleReadSlotCompletion(disk, control_file, slot_context);
            if (completion_status != AK_STATUS_SUCCESS) {
                return completion_status;
            }

            progressed = TRUE;
        }
    } while (progressed);

    return AK_STATUS_SUCCESS;
}

static AK_STATUS AkDiskPostWriteSlotAsync(
    AK_DISK* disk,
    HANDLE control_file,
    UINT64 session_id,
    AK_WRITE_SLOT_CONTEXT* slot_context)
{
    AK_STATUS status;
    UINT64 slot_id;

    slot_id = AkSessionAllocateTxId(disk->Session);
    if (slot_id == 0ull) {
        return AK_STATUS_UNSUCCESSFUL;
    }

    slot_context->SlotId = slot_id;
    if (slot_context->SlotBuffer != NULL) {
        (void)memset(slot_context->SlotBuffer, 0, slot_context->SlotBufferCapacity);
    }

    status = AkProtocolPreparePostWriteSlot(
        &slot_context->RequestBuffer,
        session_id,
        disk->Params.TargetId,
        slot_id);
    if (status != AK_STATUS_SUCCESS) {
        slot_context->SlotId = 0ull;
        return status;
    }

    AkDiskRecordWriteSlotPost(disk);
    status = AkProtocolAsyncIoBegin(
        control_file,
        slot_context->RequestBuffer.Message,
        slot_context->RequestBuffer.Size,
        slot_context->SlotBuffer,
        slot_context->SlotBufferCapacity,
        &slot_context->AsyncIo);
    if (status != AK_STATUS_SUCCESS) {
        slot_context->SlotId = 0ull;
        if (AkDiskIsRecoverableSlotStatus(status)) {
            slot_context->RetryTick = AkDiskGetRetryTick();
            return AK_STATUS_SUCCESS;
        }

        return status;
    }

    slot_context->Active = TRUE;
    slot_context->RetryTick = 0u;
    return AK_STATUS_SUCCESS;
}

static AK_STATUS AkDiskHandleWriteSlotCompletion(
    AK_DISK* disk,
    HANDLE control_file,
    AK_WRITE_SLOT_CONTEXT* slot_context)
{
    AK_STATUS status;
    DWORD bytes_transferred;
    const YUMEDISK_WRITE_SLOT_HEADER* slot_header;
    YUMEDISK_WRITE_ACK_RANGE range;

    slot_context->Active = FALSE;
    slot_context->SlotId = 0ull;

    status = AkProtocolAsyncIoFinish(
        control_file,
        &slot_context->AsyncIo,
        &bytes_transferred);
    if (status != AK_STATUS_SUCCESS) {
        if (AkDiskIsStopRequested(disk)) {
            return AK_STATUS_SUCCESS;
        }

        if (AkDiskIsRecoverableSlotStatus(status)) {
            slot_context->RetryTick = AkDiskGetRetryTick();
            return AK_STATUS_SUCCESS;
        }

        return status;
    }

    (void)bytes_transferred;
    slot_header = (const YUMEDISK_WRITE_SLOT_HEADER*)slot_context->SlotBuffer;
    status = AkDiskValidateWriteSlotHeader(disk, slot_header, slot_context->SlotBufferCapacity);
    if (status != AK_STATUS_SUCCESS) {
        AkDiskRecordProtocolFailure(disk, status);
        return AK_STATUS_SUCCESS;
    }

    status = AkDiskStageWriteSlot(disk, slot_header);
    range.EventId = slot_header->EventId;
    range.SeqBase = slot_header->Seq;
    range.SeqCount = 1u;
    range.IoStatus = status;
    range.Reserved = 0u;

    status = AkDiskEnqueueWriteAckRange(disk, &range);
    if (status != AK_STATUS_SUCCESS) {
        return status;
    }

    AkDiskRecordWriteSlotCompletion(disk);
    return AK_STATUS_SUCCESS;
}

static AK_STATUS AkDiskApplyWriteAckResult(
    AK_DISK* disk,
    const YUMEDISK_WRITE_ACK_RANGE* range,
    AK_STATUS range_status)
{
    AK_WRITE_EVENT_RECORD* record;
    AK_STATUS status;
    BOOLEAN remove_record;
    AK_WRITE_EVENT_RECORD** link;
    AK_STATUS final_status;
    BOOLEAN final_failed;

    if ((disk == NULL) || (range == NULL) || (range->EventId == 0ull) ||
        (range->SeqCount == 0u)) {
        return AK_STATUS_INVALID_PARAMETER;
    }

    AcquireSRWLockExclusive(&disk->WriteTrackLock);

    record = AkDiskFindActiveWriteEventLocked(disk, range->EventId);
    if (record == NULL) {
        record = AkDiskFindFinalizedWriteEventLocked(disk, range->EventId);
        if (record != NULL) {
            ReleaseSRWLockExclusive(&disk->WriteTrackLock);
            return AK_STATUS_SUCCESS;
        }

        ReleaseSRWLockExclusive(&disk->WriteTrackLock);
        return AK_STATUS_NOT_FOUND;
    }

    if ((UINT64)range->SeqBase + (UINT64)range->SeqCount > (UINT64)record->TotalSeq) {
        ReleaseSRWLockExclusive(&disk->WriteTrackLock);
        return AK_STATUS_INVALID_PARAMETER;
    }

    if (!record->FinalEventQueued) {
        if (range_status != AK_STATUS_SUCCESS) {
            record->FinalFailed = TRUE;
            record->FinalStatus = range_status;
        }

        record->AckedSeqCount += range->SeqCount;
        if (record->AckedSeqCount > record->TotalSeq) {
            ReleaseSRWLockExclusive(&disk->WriteTrackLock);
            return AK_STATUS_INVALID_PARAMETER;
        }
    }

    remove_record = FALSE;
    final_failed = FALSE;
    final_status = AK_STATUS_SUCCESS;

    if (!record->FinalEventQueued) {
        if (record->FinalFailed) {
            final_failed = TRUE;
            final_status = record->FinalStatus;
            status = AkDiskFinalizeWriteEventLocked(disk, record, final_status, TRUE);
            if (status != AK_STATUS_SUCCESS) {
                ReleaseSRWLockExclusive(&disk->WriteTrackLock);
                return status;
            }
            remove_record = TRUE;
        } else if (record->AckedSeqCount == record->TotalSeq) {
            status = AkDiskFinalizeWriteEventLocked(disk, record, AK_STATUS_SUCCESS, FALSE);
            if (status != AK_STATUS_SUCCESS) {
                ReleaseSRWLockExclusive(&disk->WriteTrackLock);
                return status;
            }
            remove_record = TRUE;
        }
    } else {
        remove_record = TRUE;
        final_failed = record->FinalFailed;
        final_status = record->FinalStatus;
    }

    if (!remove_record) {
        ReleaseSRWLockExclusive(&disk->WriteTrackLock);
        return AK_STATUS_SUCCESS;
    }

    link = &disk->ActiveWriteEvents;
    while (*link != NULL) {
        if (*link == record) {
            *link = record->Next;
            break;
        }
        link = &(*link)->Next;
    }

    status = AkDiskRememberFinalizedWriteEventLocked(
        disk,
        record->EventId,
        record->TotalSeq,
        final_status,
        final_failed);
    AkFree(record);
    ReleaseSRWLockExclusive(&disk->WriteTrackLock);
    return status;
}

static AK_STATUS AkDiskBeginWriteAckFlushAsync(
    AK_DISK* disk,
    HANDLE control_file,
    UINT64 session_id,
    AK_WRITE_ACK_FLUSH_CONTEXT* ack_context)
{
    AK_STATUS status;

    status = AkProtocolPrepareWriteAckBatch(
        &ack_context->RequestBuffer,
        session_id,
        disk->Params.TargetId,
        ack_context->Ranges,
        ack_context->RangeCount);
    if (status != AK_STATUS_SUCCESS) {
        return status;
    }

    status = AkProtocolAsyncIoBegin(
        control_file,
        ack_context->RequestBuffer.Message,
        ack_context->RequestBuffer.Size,
        ack_context->RequestBuffer.Message,
        ack_context->RequestBuffer.Size,
        &ack_context->AsyncIo);
    if (status != AK_STATUS_SUCCESS) {
        if (AkDiskIsRecoverableWriteAckStatus(status)) {
            ack_context->RetryTick = AkDiskGetRetryTick();
            return AK_STATUS_SUCCESS;
        }

        return status;
    }

    ack_context->Active = TRUE;
    ack_context->RetryTick = 0u;
    return AK_STATUS_SUCCESS;
}

static AK_STATUS AkDiskHandleWriteAckFlushCompletion(
    AK_DISK* disk,
    HANDLE control_file,
    AK_WRITE_ACK_FLUSH_CONTEXT* ack_context)
{
    AK_STATUS status;
    DWORD bytes_transferred;
    PYUMEDISK_MESSAGE message;
    PYUMEDISK_WRITE_ACK_BATCH_RESULT result;
    UINT32 failure_count;
    UINT32 failure_index;
    UINT32 range_index;

    ack_context->Active = FALSE;

    status = AkProtocolAsyncIoFinish(
        control_file,
        &ack_context->AsyncIo,
        &bytes_transferred);
    if (status != AK_STATUS_SUCCESS) {
        if (AkDiskIsStopRequested(disk)) {
            AkDiskResetWriteAckFlushPayload(ack_context);
            return AK_STATUS_SUCCESS;
        }

        if (AkDiskIsRecoverableWriteAckStatus(status)) {
            ack_context->RetryTick = AkDiskGetRetryTick();
            return AK_STATUS_SUCCESS;
        }

        return status;
    }

    (void)bytes_transferred;
    message = ack_context->RequestBuffer.Message;
    if (message->Header.Status != AK_STATUS_SUCCESS) {
        status = message->Header.Status;
        if (AkDiskIsRecoverableWriteAckStatus(status)) {
            ack_context->RetryTick = AkDiskGetRetryTick();
            return AK_STATUS_SUCCESS;
        }

        return status;
    }

    failure_count = 0u;
    if (message->Header.PayloadLength != 0u) {
        if (message->Header.PayloadLength < (ULONG)YUMEDISK_WRITE_ACK_BATCH_RESULT_BASE_SIZE) {
            return AK_STATUS_INVALID_PARAMETER;
        }

        result = (PYUMEDISK_WRITE_ACK_BATCH_RESULT)message->Payload;
        if ((ULONG)result->FailureCount >
            ((message->Header.PayloadLength - (ULONG)YUMEDISK_WRITE_ACK_BATCH_RESULT_BASE_SIZE) /
             (ULONG)sizeof(YUMEDISK_WRITE_ACK_FAILURE))) {
            return AK_STATUS_INVALID_PARAMETER;
        }

        failure_count = result->FailureCount;
    }

    for (range_index = 0u; range_index < ack_context->RangeCount; ++range_index) {
        AK_STATUS range_status;
        BOOLEAN failed;

        range_status = ack_context->Ranges[range_index].IoStatus;
        failed = FALSE;

        if (failure_count != 0u) {
            result = (PYUMEDISK_WRITE_ACK_BATCH_RESULT)message->Payload;
            for (failure_index = 0u; failure_index < failure_count; ++failure_index) {
                if (result->Failures[failure_index].RangeIndex == range_index) {
                    range_status = result->Failures[failure_index].Status;
                    failed = TRUE;
                    break;
                }
            }
        }

        status = AkDiskApplyWriteAckResult(disk, &ack_context->Ranges[range_index], range_status);
        if (status != AK_STATUS_SUCCESS) {
            return status;
        }

        (void)failed;
    }

    if (failure_count != 0u) {
        AkDiskRecordWriteAckRangeFailures(disk, failure_count);
    }
    AkDiskRecordWriteAckFlush(disk, ack_context->RangeCount);
    AkDiskResetWriteAckFlushPayload(ack_context);
    return AK_STATUS_SUCCESS;
}

static void AkDiskCancelActiveWriteSlots(
    AK_DISK* disk,
    HANDLE control_file,
    UINT64 session_id,
    AK_WRITE_SLOT_CONTEXT* slot_contexts,
    UINT32 slot_count)
{
    UINT32 index;

    for (index = 0u; index < slot_count; ++index) {
        if (slot_contexts[index].Active && (slot_contexts[index].SlotId != 0ull)) {
            (void)AkProtocolCancelSlot(
                control_file,
                session_id,
                disk->Params.TargetId,
                YumeDiskSlotTypeWrite,
                slot_contexts[index].SlotId);
        }
    }
}

static void AkDiskCancelActiveWriteAckFlush(
    HANDLE control_file,
    AK_WRITE_ACK_FLUSH_CONTEXT* ack_context)
{
    if ((ack_context != NULL) && ack_context->Active) {
        (void)CancelIoEx(control_file, &ack_context->AsyncIo.Overlapped);
    }
}

static void AkDiskCloseWriteContexts(
    AK_WRITE_SLOT_CONTEXT* slot_contexts,
    UINT32 slot_count)
{
    UINT32 index;

    if (slot_contexts == NULL) {
        return;
    }

    for (index = 0u; index < slot_count; ++index) {
        AkDiskDestroyWriteSlotContext(&slot_contexts[index]);
    }
}

static AK_STATUS AkDiskDrainCompletedWritePlane(
    AK_DISK* disk,
    HANDLE control_file,
    AK_WRITE_SLOT_CONTEXT* slot_contexts,
    UINT32 slot_count,
    AK_WRITE_ACK_FLUSH_CONTEXT* ack_context)
{
    UINT32 index;
    BOOLEAN progressed;

    do {
        progressed = FALSE;

        if ((ack_context != NULL) && ack_context->Active &&
            (WaitForSingleObject(ack_context->AsyncIo.Overlapped.hEvent, 0u) == WAIT_OBJECT_0)) {
            AK_STATUS completion_status;

            completion_status = AkDiskHandleWriteAckFlushCompletion(disk, control_file, ack_context);
            if (completion_status != AK_STATUS_SUCCESS) {
                return completion_status;
            }

            progressed = TRUE;
        }

        for (index = 0u; index < slot_count; ++index) {
            AK_STATUS completion_status;

            if (!slot_contexts[index].Active) {
                continue;
            }

            if (WaitForSingleObject(slot_contexts[index].AsyncIo.Overlapped.hEvent, 0u) != WAIT_OBJECT_0) {
                continue;
            }

            completion_status = AkDiskHandleWriteSlotCompletion(disk, control_file, &slot_contexts[index]);
            if (completion_status != AK_STATUS_SUCCESS) {
                return completion_status;
            }

            progressed = TRUE;
        }
    } while (progressed);

    return AK_STATUS_SUCCESS;
}

static DWORD WINAPI AkDiskWriteWorkerThreadProc(
    LPVOID context)
{
    AK_DISK_WORKER_CONTEXT* worker;
    AK_DISK* disk;
    AK_WRITE_SLOT_CONTEXT* slot_contexts;
    HANDLE control_file;
    UINT64 session_id;
    AK_STATUS status;
    UINT32 index;
    UINT32 slot_capacity;
    BOOLEAN shutting_down;
    BOOLEAN cancel_issued;
    HANDLE* wait_handles;

    worker = (AK_DISK_WORKER_CONTEXT*)context;
    if ((worker == NULL) || (worker->Disk == NULL) || (worker->SlotCount == 0u)) {
        return 0u;
    }

    disk = worker->Disk;
    slot_contexts = NULL;
    wait_handles = NULL;
    slot_capacity = (UINT32)(YUMEDISK_WRITE_SLOT_HEADER_BASE_SIZE + disk->Params.WriteSlotBytes);
    shutting_down = FALSE;
    cancel_issued = FALSE;

    status = AkSessionAcquireTransport(disk->Session, &control_file, &session_id);
    if (status != AK_STATUS_SUCCESS) {
        AkDiskRecordCommandFailure(disk, status);
        AkDiskSetBroken(disk, status);
        return 0u;
    }

    slot_contexts = (AK_WRITE_SLOT_CONTEXT*)AkAllocZero(
        (size_t)worker->SlotCount * sizeof(AK_WRITE_SLOT_CONTEXT));
    wait_handles = (HANDLE*)AkAllocZero((size_t)worker->SlotCount * sizeof(HANDLE));
    if ((slot_contexts == NULL) || (wait_handles == NULL)) {
        AkDiskRecordCommandFailure(disk, AK_STATUS_INSUFFICIENT_RESOURCES);
        AkDiskSetBroken(disk, AK_STATUS_INSUFFICIENT_RESOURCES);
        AkDiskCloseWriteContexts(slot_contexts, worker->SlotCount);
        AkFree(slot_contexts);
        AkFree(wait_handles);
        return 0u;
    }

    for (index = 0u; index < worker->SlotCount; ++index) {
        status = AkDiskInitializeWriteSlotContext(&slot_contexts[index], slot_capacity);
        if (status != AK_STATUS_SUCCESS) {
            AkDiskRecordCommandFailure(disk, status);
            AkDiskSetBroken(disk, status);
            AkDiskCloseWriteContexts(slot_contexts, worker->SlotCount);
            AkFree(slot_contexts);
            AkFree(wait_handles);
            return 0u;
        }
    }

    for (;;) {
        UINT32 wait_count;
        DWORD wait_status;

        wait_count = 0u;

        if (!shutting_down && !AkDiskIsStopRequested(disk)) {
            for (index = 0u; index < worker->SlotCount; ++index) {
                if (slot_contexts[index].Active ||
                    !AkDiskIsRetryReady(slot_contexts[index].RetryTick)) {
                    continue;
                }

                status = AkDiskPostWriteSlotAsync(
                    disk,
                    control_file,
                    session_id,
                    &slot_contexts[index]);
                if (status != AK_STATUS_SUCCESS) {
                    AkDiskRecordCommandFailure(disk, status);
                    AkDiskSetBroken(disk, status);
                    shutting_down = TRUE;
                    break;
                }
            }
        } else {
            shutting_down = TRUE;
            if (!cancel_issued) {
                AkDiskCancelActiveWriteSlots(
                    disk,
                    control_file,
                    session_id,
                    slot_contexts,
                    worker->SlotCount);
                cancel_issued = TRUE;
            }
        }

        status = AkDiskDrainCompletedWritePlane(
            disk,
            control_file,
            slot_contexts,
            worker->SlotCount,
            NULL);
        if (status != AK_STATUS_SUCCESS) {
            AkDiskRecordCommandFailure(disk, status);
            AkDiskSetBroken(disk, status);
            shutting_down = TRUE;
        }

        for (index = 0u; index < worker->SlotCount; ++index) {
            if (slot_contexts[index].Active) {
                wait_handles[wait_count] = slot_contexts[index].AsyncIo.Overlapped.hEvent;
                wait_count += 1u;
            }
        }

        if (shutting_down && (wait_count == 0u)) {
            break;
        }

        if (wait_count == 0u) {
            Sleep(AK_DISK_SLOT_ENGINE_POLL_MS);
            continue;
        }

        wait_status = WaitForMultipleObjects(
            wait_count,
            wait_handles,
            FALSE,
            AK_DISK_SLOT_ENGINE_POLL_MS);
        if ((wait_status == WAIT_TIMEOUT) ||
            (wait_status < (WAIT_OBJECT_0 + wait_count))) {
            continue;
        }

        if (wait_status == WAIT_FAILED) {
            status = AkFromWin32Error(GetLastError());
            if (!shutting_down) {
                AkDiskRecordCommandFailure(disk, status);
                AkDiskSetBroken(disk, status);
                shutting_down = TRUE;
            }
        }
    }

    AkDiskCloseWriteContexts(slot_contexts, worker->SlotCount);
    AkFree(slot_contexts);
    AkFree(wait_handles);
    return 0u;
}

static DWORD WINAPI AkDiskWriteAckFlusherThreadProc(
    LPVOID context)
{
    AK_DISK_WORKER_CONTEXT* worker;
    AK_DISK* disk;
    HANDLE control_file;
    UINT64 session_id;
    AK_STATUS status;
    AK_WRITE_ACK_FLUSH_CONTEXT ack_context;
    BOOLEAN shutting_down;
    BOOLEAN cancel_issued;
    DWORD wait_status;

    worker = (AK_DISK_WORKER_CONTEXT*)context;
    if ((worker == NULL) || (worker->Disk == NULL)) {
        return 0u;
    }

    disk = worker->Disk;
    (void)memset(&ack_context, 0, sizeof(ack_context));
    shutting_down = FALSE;
    cancel_issued = FALSE;

    status = AkSessionAcquireTransport(disk->Session, &control_file, &session_id);
    if (status != AK_STATUS_SUCCESS) {
        AkDiskRecordCommandFailure(disk, status);
        AkDiskSetBroken(disk, status);
        return 0u;
    }

    status = AkDiskInitializeWriteAckFlushContext(&ack_context, disk->Params.AckBatchMaxRanges);
    if (status != AK_STATUS_SUCCESS) {
        AkDiskRecordCommandFailure(disk, status);
        AkDiskSetBroken(disk, status);
        return 0u;
    }

    for (;;) {
        if (!shutting_down && !AkDiskIsStopRequested(disk)) {
            if (!ack_context.Active &&
                !ack_context.HasPayload &&
                AkDiskIsRetryReady(ack_context.RetryTick) &&
                (AkDiskQueryPendingWriteAckCount(disk) != 0u)) {
                ack_context.RangeCount = AkDiskStealWriteAckRanges(
                    disk,
                    ack_context.Ranges,
                    ack_context.RangeCapacity);
                ack_context.HasPayload = (BOOLEAN)(ack_context.RangeCount != 0u);
            }

            if (ack_context.HasPayload &&
                !ack_context.Active &&
                AkDiskIsRetryReady(ack_context.RetryTick)) {
                status = AkDiskBeginWriteAckFlushAsync(disk, control_file, session_id, &ack_context);
                if (status != AK_STATUS_SUCCESS) {
                    AkDiskRecordCommandFailure(disk, status);
                    AkDiskSetBroken(disk, status);
                    shutting_down = TRUE;
                }
            }
        } else {
            shutting_down = TRUE;
            if (!ack_context.Active) {
                AkDiskResetWriteAckFlushPayload(&ack_context);
            }
            if (!cancel_issued) {
                AkDiskClearPendingWriteAcks(disk);
                AkDiskCancelActiveWriteAckFlush(control_file, &ack_context);
                cancel_issued = TRUE;
            }
        }

        status = AkDiskDrainCompletedWritePlane(
            disk,
            control_file,
            NULL,
            0u,
            &ack_context);
        if (status != AK_STATUS_SUCCESS) {
            AkDiskRecordCommandFailure(disk, status);
            AkDiskSetBroken(disk, status);
            shutting_down = TRUE;
        }

        if (shutting_down && !ack_context.Active) {
            break;
        }

        if (!ack_context.Active) {
            HANDLE wait_handle;

            wait_handle = disk->WriteAckWakeEvent;
            if (wait_handle == NULL) {
                Sleep(AK_DISK_SLOT_ENGINE_POLL_MS);
                continue;
            }

            wait_status = WaitForSingleObject(wait_handle, AK_DISK_SLOT_ENGINE_POLL_MS);
            if ((wait_status == WAIT_OBJECT_0) || (wait_status == WAIT_TIMEOUT)) {
                continue;
            }

            status = AkFromWin32Error(GetLastError());
            if (!shutting_down) {
                AkDiskRecordCommandFailure(disk, status);
                AkDiskSetBroken(disk, status);
                shutting_down = TRUE;
            }
            continue;
        }

        wait_status = WaitForSingleObject(
            ack_context.AsyncIo.Overlapped.hEvent,
            AK_DISK_SLOT_ENGINE_POLL_MS);
        if ((wait_status == WAIT_TIMEOUT) || (wait_status == WAIT_OBJECT_0)) {
            continue;
        }

        if (wait_status == WAIT_FAILED) {
            status = AkFromWin32Error(GetLastError());
            if (!shutting_down) {
                AkDiskRecordCommandFailure(disk, status);
                AkDiskSetBroken(disk, status);
                shutting_down = TRUE;
            }
        }
    }

    AkDiskClearPendingWriteAcks(disk);
    AkDiskDestroyWriteAckFlushContext(&ack_context);
    return 0u;
}

static void AkDiskCancelActiveReadSlots(
    AK_DISK* disk,
    HANDLE control_file,
    UINT64 session_id,
    AK_READ_SLOT_CONTEXT* slot_contexts,
    UINT32 slot_count)
{
    UINT32 index;

    for (index = 0u; index < slot_count; ++index) {
        AK_READ_SLOT_CONTEXT* slot_context;

        slot_context = &slot_contexts[index];
        if (slot_context->Active && (slot_context->SlotId != 0ull)) {
            (void)AkProtocolCancelSlot(
                control_file,
                session_id,
                disk->Params.TargetId,
                YumeDiskSlotTypeRead,
                slot_context->SlotId);
        }
    }
}

static void AkDiskCancelActiveReadAcks(
    HANDLE control_file,
    AK_READ_ACK_CONTEXT* ack_contexts,
    UINT32 slot_count)
{
    UINT32 index;

    for (index = 0u; index < slot_count; ++index) {
        AK_READ_ACK_CONTEXT* ack_context;

        ack_context = &ack_contexts[index];
        if (ack_context->Active) {
            (void)CancelIoEx(control_file, &ack_context->AsyncIo.Overlapped);
        }
    }
}

static void AkDiskCloseReadContexts(
    AK_READ_SLOT_CONTEXT* slot_contexts,
    AK_READ_ACK_CONTEXT* ack_contexts,
    UINT32 slot_count)
{
    UINT32 index;

    if (slot_contexts != NULL) {
        for (index = 0u; index < slot_count; ++index) {
            AkFree(slot_contexts[index].ReadDataBuffer);
            slot_contexts[index].ReadDataBuffer = NULL;
            slot_contexts[index].ReadDataCapacity = 0u;
            AkDiskDestroyReadSlotContext(&slot_contexts[index]);
        }
    }

    if (ack_contexts != NULL) {
        for (index = 0u; index < slot_count; ++index) {
            AkDiskDestroyReadAckContext(&ack_contexts[index]);
        }
    }
}

static DWORD WINAPI AkDiskReadWorkerThreadProc(
    LPVOID context)
{
    AK_DISK_WORKER_CONTEXT* worker;
    AK_DISK* disk;
    AK_READ_SLOT_CONTEXT* slot_contexts;
    AK_READ_ACK_CONTEXT* ack_contexts;
    HANDLE control_file;
    UINT64 session_id;
    AK_STATUS status;
    UINT32 index;
    BOOLEAN shutting_down;
    BOOLEAN cancel_issued;
    HANDLE* wait_handles;

    worker = (AK_DISK_WORKER_CONTEXT*)context;
    if ((worker == NULL) || (worker->Disk == NULL) || (worker->SlotCount == 0u)) {
        return 0u;
    }

    disk = worker->Disk;
    slot_contexts = NULL;
    ack_contexts = NULL;
    wait_handles = NULL;
    shutting_down = FALSE;
    cancel_issued = FALSE;

    status = AkSessionAcquireTransport(disk->Session, &control_file, &session_id);
    if (status != AK_STATUS_SUCCESS) {
        AkDiskRecordCommandFailure(disk, status);
        AkDiskSetBroken(disk, status);
        return 0u;
    }

    slot_contexts = (AK_READ_SLOT_CONTEXT*)AkAllocZero(
        (size_t)worker->SlotCount * sizeof(AK_READ_SLOT_CONTEXT));
    ack_contexts = (AK_READ_ACK_CONTEXT*)AkAllocZero(
        (size_t)worker->SlotCount * sizeof(AK_READ_ACK_CONTEXT));
    wait_handles = (HANDLE*)AkAllocZero((size_t)(worker->SlotCount * 2u) * sizeof(HANDLE));
    if ((slot_contexts == NULL) || (ack_contexts == NULL) || (wait_handles == NULL)) {
        AkDiskRecordCommandFailure(disk, AK_STATUS_INSUFFICIENT_RESOURCES);
        AkDiskSetBroken(disk, AK_STATUS_INSUFFICIENT_RESOURCES);
        AkDiskCloseReadContexts(slot_contexts, ack_contexts, worker->SlotCount);
        AkFree(slot_contexts);
        AkFree(ack_contexts);
        AkFree(wait_handles);
        return 0u;
    }

    for (index = 0u; index < worker->SlotCount; ++index) {
        status = AkDiskInitializeReadSlotContext(&slot_contexts[index]);
        if (status != AK_STATUS_SUCCESS) {
            AkDiskRecordCommandFailure(disk, status);
            AkDiskSetBroken(disk, status);
            AkDiskCloseReadContexts(slot_contexts, ack_contexts, worker->SlotCount);
            AkFree(slot_contexts);
            AkFree(ack_contexts);
            AkFree(wait_handles);
            return 0u;
        }

        slot_contexts[index].ReadDataCapacity = disk->Params.WriteSlotBytes;
        slot_contexts[index].ReadDataBuffer = (BYTE*)AkAllocZero(disk->Params.WriteSlotBytes);
        if (slot_contexts[index].ReadDataBuffer == NULL) {
            status = AK_STATUS_INSUFFICIENT_RESOURCES;
            AkDiskRecordCommandFailure(disk, status);
            AkDiskSetBroken(disk, status);
            AkDiskCloseReadContexts(slot_contexts, ack_contexts, worker->SlotCount);
            AkFree(slot_contexts);
            AkFree(ack_contexts);
            AkFree(wait_handles);
            return 0u;
        }

        status = AkDiskInitializeReadAckContext(&ack_contexts[index]);
        if (status != AK_STATUS_SUCCESS) {
            AkDiskRecordCommandFailure(disk, status);
            AkDiskSetBroken(disk, status);
            AkDiskCloseReadContexts(slot_contexts, ack_contexts, worker->SlotCount);
            AkFree(slot_contexts);
            AkFree(ack_contexts);
            AkFree(wait_handles);
            return 0u;
        }
    }

    for (;;) {
        UINT32 wait_count;
        DWORD wait_status;

        wait_count = 0u;

        if (!shutting_down && !AkDiskIsStopRequested(disk)) {
            status = AkDiskDispatchPendingReadAcks(
                disk,
                control_file,
                session_id,
                slot_contexts,
                ack_contexts,
                worker->SlotCount);
            if (status != AK_STATUS_SUCCESS) {
                AkDiskRecordCommandFailure(disk, status);
                AkDiskSetBroken(disk, status);
                shutting_down = TRUE;
            }

            if (!shutting_down) {
                for (index = 0u; index < worker->SlotCount; ++index) {
                    AK_READ_SLOT_CONTEXT* slot_context;

                    slot_context = &slot_contexts[index];
                    if (slot_context->Active || slot_context->AckPending ||
                        !AkDiskIsRetryReady(slot_context->RetryTick)) {
                        continue;
                    }

                    status = AkDiskPostReadSlotAsync(
                        disk,
                        control_file,
                        session_id,
                        slot_context);
                    if (status != AK_STATUS_SUCCESS) {
                        AkDiskRecordCommandFailure(disk, status);
                        AkDiskSetBroken(disk, status);
                        shutting_down = TRUE;
                        break;
                    }
                }
            }
        } else {
            shutting_down = TRUE;

            for (index = 0u; index < worker->SlotCount; ++index) {
                if (!slot_contexts[index].Active) {
                    AkDiskResetReadSlotPendingState(&slot_contexts[index]);
                }
                if (!ack_contexts[index].Active) {
                    AkDiskResetReadAckPayload(&ack_contexts[index]);
                }
            }

            if (!cancel_issued) {
                AkDiskCancelActiveReadSlots(
                    disk,
                    control_file,
                    session_id,
                    slot_contexts,
                    worker->SlotCount);
                AkDiskCancelActiveReadAcks(
                    control_file,
                    ack_contexts,
                    worker->SlotCount);
                cancel_issued = TRUE;
            }
        }

        status = AkDiskDrainCompletedReadPlane(
            disk,
            control_file,
            slot_contexts,
            ack_contexts,
            worker->SlotCount);
        if (status != AK_STATUS_SUCCESS) {
            AkDiskRecordCommandFailure(disk, status);
            AkDiskSetBroken(disk, status);
            shutting_down = TRUE;
        }

        wait_count = 0u;
        for (index = 0u; index < worker->SlotCount; ++index) {
            if (slot_contexts[index].Active) {
                wait_handles[wait_count] = slot_contexts[index].AsyncIo.Overlapped.hEvent;
                wait_count += 1u;
            }
        }
        for (index = 0u; index < worker->SlotCount; ++index) {
            if (ack_contexts[index].Active) {
                wait_handles[wait_count] = ack_contexts[index].AsyncIo.Overlapped.hEvent;
                wait_count += 1u;
            }
        }

        if (shutting_down && (wait_count == 0u)) {
            break;
        }

        if (wait_count == 0u) {
            Sleep(AK_DISK_SLOT_ENGINE_POLL_MS);
            continue;
        }

        wait_status = WaitForMultipleObjects(
            wait_count,
            wait_handles,
            FALSE,
            AK_DISK_SLOT_ENGINE_POLL_MS);
        if ((wait_status == WAIT_TIMEOUT) ||
            (wait_status < (WAIT_OBJECT_0 + wait_count))) {
            continue;
        }

        if (wait_status == WAIT_FAILED) {
            status = AkFromWin32Error(GetLastError());
            if (!shutting_down) {
                AkDiskRecordCommandFailure(disk, status);
                AkDiskSetBroken(disk, status);
                shutting_down = TRUE;
            }
        }
    }

    AkDiskCloseReadContexts(slot_contexts, ack_contexts, worker->SlotCount);
    AkFree(slot_contexts);
    AkFree(ack_contexts);
    AkFree(wait_handles);
    return 0u;
}

static DWORD WINAPI AkDiskIdleWorkerThreadProc(
    LPVOID context)
{
    AK_DISK_WORKER_CONTEXT* worker;
    AK_DISK* disk;

    worker = (AK_DISK_WORKER_CONTEXT*)context;
    if ((worker == NULL) || (worker->Disk == NULL)) {
        return 0u;
    }

    disk = worker->Disk;
    if (disk->StopEvent != NULL) {
        (void)WaitForSingleObject(disk->StopEvent, INFINITE);
    }

    return 0u;
}

static AK_STATUS AkDiskStartWorkerArray(
    AK_DISK* disk,
    HANDLE* threads,
    AK_DISK_WORKER_CONTEXT* contexts,
    UINT32 worker_count,
    UINT32 worker_kind)
{
    UINT32 index;

    for (index = 0u; index < worker_count; ++index) {
        LPTHREAD_START_ROUTINE thread_proc;

        contexts[index].Disk = disk;
        contexts[index].WorkerIndex = index;
        contexts[index].WorkerKind = worker_kind;
        contexts[index].SlotCount = 0u;

        thread_proc = AkDiskIdleWorkerThreadProc;
        if (worker_kind == AK_DISK_WORKER_KIND_READ) {
            contexts[index].SlotCount = AkDiskComputeReadWorkerSlotCount(disk, index);
            if (contexts[index].SlotCount == 0u) {
                threads[index] = NULL;
                continue;
            }

            thread_proc = AkDiskReadWorkerThreadProc;
        } else if (worker_kind == AK_DISK_WORKER_KIND_WRITE) {
            contexts[index].SlotCount = AkDiskComputeWriteWorkerSlotCount(disk, index);
            if (contexts[index].SlotCount == 0u) {
                threads[index] = NULL;
                continue;
            }

            thread_proc = AkDiskWriteWorkerThreadProc;
        }

        threads[index] = CreateThread(
            NULL,
            0u,
            thread_proc,
            &contexts[index],
            0u,
            NULL);
        if (threads[index] == NULL) {
            return AkFromWin32Error(GetLastError());
        }
    }

    return AK_STATUS_SUCCESS;
}

static void AkDiskJoinWorkerArray(
    HANDLE* threads,
    UINT32 worker_count)
{
    UINT32 index;

    if (threads == NULL) {
        return;
    }

    for (index = 0u; index < worker_count; ++index) {
        if (threads[index] != NULL) {
            (void)WaitForSingleObject(threads[index], INFINITE);
            CloseHandle(threads[index]);
            threads[index] = NULL;
        }
    }
}

static AK_STATUS AkDiskStartWorkers(
    AK_DISK* disk)
{
    AK_STATUS status;

    status = AkDiskAllocateWorkerArrays(disk);
    if (status != AK_STATUS_SUCCESS) {
        return status;
    }

    status = AkDiskStartWorkerArray(
        disk,
        disk->ReadWorkerThreads,
        disk->ReadWorkerContexts,
        disk->Params.ReadWorkerCount,
        AK_DISK_WORKER_KIND_READ);
    if (status != AK_STATUS_SUCCESS) {
        AkDiskStopWorkers(disk);
        return status;
    }

    status = AkDiskStartWorkerArray(
        disk,
        disk->WriteWorkerThreads,
        disk->WriteWorkerContexts,
        disk->Params.WriteWorkerCount,
        AK_DISK_WORKER_KIND_WRITE);
    if (status != AK_STATUS_SUCCESS) {
        AkDiskStopWorkers(disk);
        return status;
    }

    disk->WriteAckWakeEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (disk->WriteAckWakeEvent == NULL) {
        status = AkFromWin32Error(GetLastError());
        AkDiskStopWorkers(disk);
        return status;
    }

    disk->AckFlusherContext.Disk = disk;
    disk->AckFlusherContext.WorkerIndex = 0u;
    disk->AckFlusherContext.WorkerKind = AK_DISK_WORKER_KIND_ACK;
    disk->AckFlusherContext.SlotCount = 0u;
    disk->AckFlusherThread = CreateThread(
        NULL,
        0u,
        AkDiskWriteAckFlusherThreadProc,
        &disk->AckFlusherContext,
        0u,
        NULL);
    if (disk->AckFlusherThread == NULL) {
        status = AkFromWin32Error(GetLastError());
        AkDiskStopWorkers(disk);
        return status;
    }

    AcquireSRWLockExclusive(&disk->Lock);
    disk->State.ReadWorkersRunning = TRUE;
    disk->State.WriteWorkersRunning = TRUE;
    disk->State.AckFlusherRunning = TRUE;
    ReleaseSRWLockExclusive(&disk->Lock);

    return AK_STATUS_SUCCESS;
}

static void AkDiskStopWorkers(
    AK_DISK* disk)
{
    if (disk->StopEvent != NULL) {
        SetEvent(disk->StopEvent);
    }
    if (disk->WriteAckWakeEvent != NULL) {
        SetEvent(disk->WriteAckWakeEvent);
    }

    AkDiskJoinWorkerArray(disk->ReadWorkerThreads, disk->Params.ReadWorkerCount);
    AkDiskJoinWorkerArray(disk->WriteWorkerThreads, disk->Params.WriteWorkerCount);

    if (disk->AckFlusherThread != NULL) {
        (void)WaitForSingleObject(disk->AckFlusherThread, INFINITE);
        CloseHandle(disk->AckFlusherThread);
        disk->AckFlusherThread = NULL;
    }

    if (disk->WriteAckWakeEvent != NULL) {
        CloseHandle(disk->WriteAckWakeEvent);
        disk->WriteAckWakeEvent = NULL;
    }

    AkDiskFreeWorkerArrays(disk);

    AcquireSRWLockExclusive(&disk->Lock);
    disk->State.ReadWorkersRunning = FALSE;
    disk->State.WriteWorkersRunning = FALSE;
    disk->State.AckFlusherRunning = FALSE;
    ReleaseSRWLockExclusive(&disk->Lock);
}

static AK_STATUS AkDiskPrimeReadAvailability(
    AK_DISK* disk)
{
    AcquireSRWLockExclusive(&disk->Lock);
    disk->PrimedReadSlotDepth = disk->Params.QueueDepth;
    ReleaseSRWLockExclusive(&disk->Lock);
    return AK_STATUS_SUCCESS;
}

static void AkDiskDestroy(
    AK_DISK* disk)
{
    AK_WRITE_EVENT_RECORD* active_events;
    AK_WRITE_EVENT_RECORD* finalized_events;

    if (disk == NULL) {
        return;
    }

    if (disk->RegisteredInSession && (disk->Session != NULL)) {
        AkSessionUnregisterDisk(disk->Session, disk);
    }

    AkDiskStopWorkers(disk);
    AkDiskClearPendingWriteAcks(disk);

    AcquireSRWLockExclusive(&disk->WriteTrackLock);
    active_events = disk->ActiveWriteEvents;
    finalized_events = disk->FinalizedWriteEventsHead;
    disk->ActiveWriteEvents = NULL;
    disk->FinalizedWriteEventsHead = NULL;
    disk->FinalizedWriteEventsTail = NULL;
    disk->FinalizedWriteEventCount = 0u;
    ReleaseSRWLockExclusive(&disk->WriteTrackLock);

    AkDiskFreeWriteEventList(active_events);
    AkDiskFreeWriteEventList(finalized_events);

    if (disk->StopEvent != NULL) {
        CloseHandle(disk->StopEvent);
        disk->StopEvent = NULL;
    }
    AkFree(disk);
}

void AkDiskDestroyDetached(
    AK_DISK* disk)
{
    AkDiskDestroy(disk);
}

static AK_STATUS AkDiskEmitLifecycleEvent(
    AK_DISK* disk,
    AK_EVENT_TYPE type,
    AK_STATUS status)
{
    AK_EVENT event_record;

    (void)memset(&event_record, 0, sizeof(event_record));
    event_record.Type = type;
    event_record.TargetId = disk->State.TargetId;
    event_record.DiskRuntimeId = disk->State.DiskRuntimeId;
    event_record.Status = status;
    return AkEventQueuePush(disk->Session, &event_record);
}

AK_STATUS AkDiskCreate(
    AK_SESSION* session,
    const AK_DISK_PARAMS* params,
    const AK_MEDIA_OPS* media_ops,
    void* media_ctx,
    AK_DISK** out_disk)
{
    AK_DISK* disk;
    HANDLE control_file;
    UINT64 session_id;
    UINT64 runtime_id;
    AK_STATUS status;

    if (out_disk == NULL) {
        return AK_STATUS_INVALID_PARAMETER;
    }

    *out_disk = NULL;

    status = AkDiskValidateParams(params, media_ops);
    if (status != AK_STATUS_SUCCESS) {
        return status;
    }

    if (session == NULL) {
        return AK_STATUS_INVALID_PARAMETER;
    }

    disk = (AK_DISK*)AkAllocZero(sizeof(*disk));
    if (disk == NULL) {
        return AK_STATUS_INSUFFICIENT_RESOURCES;
    }

    disk->Session = session;
    disk->Params = *params;
    disk->MediaOps = *media_ops;
    disk->MediaCtx = media_ctx;
    InitializeSRWLock(&disk->Lock);
    InitializeSRWLock(&disk->WriteAckLock);
    InitializeSRWLock(&disk->WriteTrackLock);
    AkDiskInitializeState(disk);

    disk->StopEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (disk->StopEvent == NULL) {
        status = AkFromWin32Error(GetLastError());
        AkDiskDestroy(disk);
        return status;
    }

    AcquireSRWLockExclusive(&disk->Lock);
    disk->State.Lifecycle = AkStateStarting;
    ReleaseSRWLockExclusive(&disk->Lock);

    status = AkSessionAcquireTransport(session, &control_file, &session_id);
    if (status != AK_STATUS_SUCCESS) {
        AkDiskSetLastError(disk, status);
        AkDiskDestroy(disk);
        return status;
    }

    status = AkSessionRegisterDisk(session, disk, &runtime_id);
    if (status != AK_STATUS_SUCCESS) {
        AkDiskSetLastError(disk, status);
        AkDiskDestroy(disk);
        return status;
    }

    (void)runtime_id;

    status = AkDiskStartWorkers(disk);
    if (status != AK_STATUS_SUCCESS) {
        AkDiskSetLastError(disk, status);
        AkSessionUnregisterDisk(session, disk);
        AkDiskDestroy(disk);
        return status;
    }

    status = AkDiskPrimeReadAvailability(disk);
    if (status != AK_STATUS_SUCCESS) {
        AkDiskSetLastError(disk, status);
        AkDiskStopWorkers(disk);
        AkSessionUnregisterDisk(session, disk);
        AkDiskDestroy(disk);
        return status;
    }

    status = AkProtocolCreateDisk(control_file, session_id, &disk->Params);
    if (status != AK_STATUS_SUCCESS) {
        AkDiskSetLastError(disk, status);
        AkDiskStopWorkers(disk);
        AkSessionUnregisterDisk(session, disk);
        AkDiskDestroy(disk);
        return status;
    }

    AcquireSRWLockExclusive(&disk->Lock);
    disk->State.Lifecycle = AkStateRunning;
    disk->State.LastError = AK_STATUS_SUCCESS;
    ReleaseSRWLockExclusive(&disk->Lock);

    status = AkDiskEmitLifecycleEvent(disk, AkEventDiskOnline, AK_STATUS_SUCCESS);
    if (status != AK_STATUS_SUCCESS) {
        AkDiskSetLastError(disk, status);
        AcquireSRWLockExclusive(&disk->Lock);
        disk->State.Lifecycle = AkStateBroken;
        ReleaseSRWLockExclusive(&disk->Lock);
        AkDiskStopWorkers(disk);
        (void)AkProtocolRemoveDisk(control_file, session_id, disk->Params.TargetId, 0u);
        AkSessionUnregisterDisk(session, disk);
        AkDiskDestroy(disk);
        return status;
    }

    *out_disk = disk;
    return AK_STATUS_SUCCESS;
}

AK_STATUS AkDiskRemove(AK_DISK* disk)
{
    HANDLE control_file;
    UINT64 session_id;
    AK_STATUS status;
    AK_STATUS remove_status;
    BOOLEAN already_closed;

    if (disk == NULL) {
        return AK_STATUS_INVALID_PARAMETER;
    }

    AcquireSRWLockExclusive(&disk->Lock);
    already_closed = (BOOLEAN)((disk->State.Lifecycle == AkStateClosed) ||
                               (disk->State.Lifecycle == AkStateRemoving));
    if (!already_closed) {
        disk->State.Lifecycle = AkStateRemoving;
        disk->State.LastError = AK_STATUS_SUCCESS;
    }
    ReleaseSRWLockExclusive(&disk->Lock);

    if (already_closed) {
        return AK_STATUS_SUCCESS;
    }

    status = AkSessionAcquireTransport(disk->Session, &control_file, &session_id);
    if (status != AK_STATUS_SUCCESS) {
        AkDiskSetLastError(disk, status);
        control_file = NULL;
        session_id = 0ull;
    }

    AkDiskStopWorkers(disk);

    remove_status = AK_STATUS_SUCCESS;
    if ((control_file != NULL) && (control_file != INVALID_HANDLE_VALUE) && (session_id != 0ull)) {
        remove_status = AkProtocolRemoveDisk(control_file, session_id, disk->Params.TargetId, 0u);
    }

    if (remove_status != AK_STATUS_SUCCESS) {
        AkDiskSetLastError(disk, remove_status);
    }

    status = AkDiskEmitLifecycleEvent(disk, AkEventDiskRemoved, remove_status);
    if (status != AK_STATUS_SUCCESS) {
        AkDiskSetLastError(disk, status);
    }

    AcquireSRWLockExclusive(&disk->Lock);
    disk->State.Lifecycle = AkStateClosed;
    ReleaseSRWLockExclusive(&disk->Lock);

    AkSessionUnregisterDisk(disk->Session, disk);
    AkDiskDestroy(disk);
    return remove_status;
}

AK_STATUS AkDiskQueryState(
    AK_DISK* disk,
    AK_DISK_STATE* out_state)
{
    if ((disk == NULL) || (out_state == NULL)) {
        return AK_STATUS_INVALID_PARAMETER;
    }

    AcquireSRWLockShared(&disk->Lock);
    *out_state = disk->State;
    ReleaseSRWLockShared(&disk->Lock);
    return AK_STATUS_SUCCESS;
}

AK_STATUS AkDiskQueryStats(
    AK_DISK* disk,
    AK_DISK_STATS* out_stats)
{
    if ((disk == NULL) || (out_stats == NULL)) {
        return AK_STATUS_INVALID_PARAMETER;
    }

    AcquireSRWLockShared(&disk->Lock);
    *out_stats = disk->Stats;
    ReleaseSRWLockShared(&disk->Lock);
    return AK_STATUS_SUCCESS;
}
