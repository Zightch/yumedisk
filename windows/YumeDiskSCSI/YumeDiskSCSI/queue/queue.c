#include "queue.h"

#include "..\core\memory.h"

static
BOOLEAN
DiskEventHasInlineData(
    _In_ const YUMEDISK_EVENT_NODE* EventNode
)
{
    return EventNode->Event.EventType == YumeDiskEventWriteRequest &&
        EventNode->Event.DataLength != 0;
}

static
ULONG
DiskGetEventPayloadLength(
    _In_ const YUMEDISK_EVENT_NODE* EventNode
)
{
    ULONG payloadLength;

    payloadLength = sizeof(YUMEDISK_EVENT);
    if (DiskEventHasInlineData(EventNode)) {
        payloadLength += EventNode->Event.DataLength;
    }

    return payloadLength;
}

static
NTSTATUS
DiskFillWaitEventMessage(
    _Inout_ PYUMEDISK_MESSAGE Message,
    _In_ ULONG MessageCapacity,
    _In_ const YUMEDISK_EVENT_NODE* EventNode
)
{
    ULONG payloadLength;

    payloadLength = DiskGetEventPayloadLength(EventNode);
    if (MessageCapacity < (ULONG)(YUMEDISK_MESSAGE_BASE_SIZE + payloadLength)) {
        return STATUS_BUFFER_TOO_SMALL;
    }

    DiskInitMessageStatus(Message, YumeDiskCommandWaitEvent, STATUS_SUCCESS, payloadLength);
    RtlCopyMemory(Message->Payload, &EventNode->Event, sizeof(YUMEDISK_EVENT));

    if (DiskEventHasInlineData(EventNode)) {
        PYUMEDISK_PENDING_IO_NODE pendingIo;

        pendingIo = EventNode->PendingIo;
        if (pendingIo == NULL || pendingIo->Srb == NULL || pendingIo->Srb->DataBuffer == NULL) {
            return STATUS_DEVICE_DATA_ERROR;
        }

        RtlCopyMemory(
            Message->Payload + sizeof(YUMEDISK_EVENT),
            pendingIo->Srb->DataBuffer,
            EventNode->Event.DataLength);
    }

    return STATUS_SUCCESS;
}

static
BOOLEAN
DiskTryCompleteWaiterWithEvent(
    _In_ PVOID DeviceExtension,
    _In_ PYUMEDISK_WAITER_NODE WaiterNode,
    _In_ const YUMEDISK_EVENT_NODE* EventNode
)
{
    PSTORAGE_REQUEST_BLOCK waiterSrb;
    PSRB_IO_CONTROL srbIoControl;
    PYUMEDISK_MESSAGE message;
    ULONG messageCapacity;
    NTSTATUS status;

    waiterSrb = WaiterNode->Srb;
    if (waiterSrb == NULL || waiterSrb->DataBuffer == NULL) {
        DiskFree(WaiterNode);
        return FALSE;
    }

    srbIoControl = (PSRB_IO_CONTROL)waiterSrb->DataBuffer;
    message = (PYUMEDISK_MESSAGE)(srbIoControl + 1);
    messageCapacity = srbIoControl->Length;
    status = DiskFillWaitEventMessage(message, messageCapacity, EventNode);
    if (!NT_SUCCESS(status)) {
        DiskInitMessageStatus(message, YumeDiskCommandWaitEvent, status, 0);
        DiskCompleteIoctlSrb(waiterSrb, srbIoControl, status, message->Header.Size);
        StorPortNotification(RequestComplete, DeviceExtension, waiterSrb);
        DiskFree(WaiterNode);
        return FALSE;
    }

    DiskCompleteIoctlSrb(waiterSrb, srbIoControl, STATUS_SUCCESS, message->Header.Size);
    StorPortNotification(RequestComplete, DeviceExtension, waiterSrb);
    DiskFree(WaiterNode);
    return TRUE;
}

static
VOID
DiskCompleteWaiterWithStatus(
    _In_ PVOID DeviceExtension,
    _In_ PYUMEDISK_WAITER_NODE WaiterNode,
    _In_ NTSTATUS Status
)
{
    PSTORAGE_REQUEST_BLOCK waiterSrb;
    PSRB_IO_CONTROL srbIoControl;
    PYUMEDISK_MESSAGE message;

    waiterSrb = WaiterNode->Srb;
    if (waiterSrb == NULL || waiterSrb->DataBuffer == NULL) {
        DiskFree(WaiterNode);
        return;
    }

    srbIoControl = (PSRB_IO_CONTROL)waiterSrb->DataBuffer;
    message = (PYUMEDISK_MESSAGE)(srbIoControl + 1);
    DiskInitMessageStatus(message, YumeDiskCommandWaitEvent, Status, 0);
    DiskCompleteIoctlSrb(waiterSrb, srbIoControl, Status, message->Header.Size);
    StorPortNotification(RequestComplete, DeviceExtension, waiterSrb);
    DiskFree(WaiterNode);
}

static
VOID
DiskQueueOrDeliverEventNode(
    _In_ PVOID DeviceExtension,
    _In_ PYUMEDISK_EVENT_NODE EventNode
)
{
    PDEVICE_CONTEXT extension;
    KIRQL oldIrql;
    PYUMEDISK_WAITER_NODE waiterNode;

    extension = (PDEVICE_CONTEXT)DeviceExtension;
    waiterNode = NULL;
    if (EventNode == NULL) {
        return;
    }

    KeAcquireSpinLock(&extension->ControlLock, &oldIrql);
    if (!IsListEmpty(&extension->PendingWaiters)) {
        PLIST_ENTRY entry;

        entry = RemoveHeadList(&extension->PendingWaiters);
        extension->PendingWaiterCount--;
        waiterNode = CONTAINING_RECORD(entry, YUMEDISK_WAITER_NODE, Link);
    } else {
        InsertTailList(&extension->PendingEvents, &EventNode->Link);
        extension->PendingEventCount++;
    }
    KeReleaseSpinLock(&extension->ControlLock, oldIrql);

    if (waiterNode != NULL) {
        if (DiskTryCompleteWaiterWithEvent(DeviceExtension, waiterNode, EventNode)) {
            DiskFree(EventNode);
        } else {
            KeAcquireSpinLock(&extension->ControlLock, &oldIrql);
            InsertHeadList(&extension->PendingEvents, &EventNode->Link);
            extension->PendingEventCount++;
            KeReleaseSpinLock(&extension->ControlLock, oldIrql);
        }
    }
}

static
VOID
DiskRemoveQueuedEventsForPendingIoLocked(
    _Inout_ PDEVICE_CONTEXT Extension,
    _In_ PYUMEDISK_PENDING_IO_NODE PendingIo
)
{
    PLIST_ENTRY entry;

    entry = Extension->PendingEvents.Flink;
    while (entry != &Extension->PendingEvents) {
        PLIST_ENTRY nextEntry;
        PYUMEDISK_EVENT_NODE eventNode;

        nextEntry = entry->Flink;
        eventNode = CONTAINING_RECORD(entry, YUMEDISK_EVENT_NODE, Link);
        if (eventNode->PendingIo == PendingIo) {
            RemoveEntryList(entry);
            Extension->PendingEventCount--;
            DiskFree(eventNode);
        }

        entry = nextEntry;
    }
}

static
VOID
DiskRemoveQueuedIoEventsByTargetLocked(
    _Inout_ PDEVICE_CONTEXT Extension,
    _In_ ULONG TargetId
)
{
    PLIST_ENTRY entry;

    entry = Extension->PendingEvents.Flink;
    while (entry != &Extension->PendingEvents) {
        PLIST_ENTRY nextEntry;
        PYUMEDISK_EVENT_NODE eventNode;

        nextEntry = entry->Flink;
        eventNode = CONTAINING_RECORD(entry, YUMEDISK_EVENT_NODE, Link);
        if (eventNode->PendingIo != NULL && eventNode->PendingIo->TargetId == TargetId) {
            RemoveEntryList(entry);
            Extension->PendingEventCount--;
            DiskFree(eventNode);
        }

        entry = nextEntry;
    }
}

static
PYUMEDISK_PENDING_IO_NODE
DiskDetachPendingIoByTxIdLocked(
    _Inout_ PDEVICE_CONTEXT Extension,
    _In_ ULONGLONG TxId
)
{
    PLIST_ENTRY entry;

    entry = Extension->PendingIo.Flink;
    while (entry != &Extension->PendingIo) {
        PYUMEDISK_PENDING_IO_NODE pendingIo;

        pendingIo = CONTAINING_RECORD(entry, YUMEDISK_PENDING_IO_NODE, Link);
        if (pendingIo->TxId == TxId) {
            RemoveEntryList(entry);
            Extension->PendingIoCount--;
            return pendingIo;
        }

        entry = entry->Flink;
    }

    return NULL;
}

static
PYUMEDISK_PENDING_IO_NODE
DiskDetachPendingIoByTargetLocked(
    _Inout_ PDEVICE_CONTEXT Extension,
    _In_ ULONG TargetId
)
{
    PLIST_ENTRY entry;

    entry = Extension->PendingIo.Flink;
    while (entry != &Extension->PendingIo) {
        PYUMEDISK_PENDING_IO_NODE pendingIo;

        pendingIo = CONTAINING_RECORD(entry, YUMEDISK_PENDING_IO_NODE, Link);
        if (pendingIo->TargetId == TargetId) {
            RemoveEntryList(entry);
            Extension->PendingIoCount--;
            return pendingIo;
        }

        entry = entry->Flink;
    }

    return NULL;
}

VOID
DiskCancelPendingIoByTarget(
    _In_ PVOID DeviceExtension,
    _Inout_ PDEVICE_CONTEXT Extension,
    _In_ ULONG TargetId,
    _In_ NTSTATUS Status
)
{
    KIRQL oldIrql;

    KeAcquireSpinLock(&Extension->ControlLock, &oldIrql);
    DiskRemoveQueuedIoEventsByTargetLocked(Extension, TargetId);
    KeReleaseSpinLock(&Extension->ControlLock, oldIrql);

    for (;;) {
        PYUMEDISK_PENDING_IO_NODE pendingIo;

        KeAcquireSpinLock(&Extension->ControlLock, &oldIrql);
        pendingIo = DiskDetachPendingIoByTargetLocked(Extension, TargetId);
        KeReleaseSpinLock(&Extension->ControlLock, oldIrql);
        if (pendingIo == NULL) {
            break;
        }

        DiskCompleteScsiSrb(DeviceExtension, pendingIo, Status, NULL, 0);
    }
}

static
VOID
DiskCancelAllPendingIo(
    _In_ PVOID DeviceExtension,
    _Inout_ PDEVICE_CONTEXT Extension,
    _In_ NTSTATUS Status
)
{
    KIRQL oldIrql;

    KeAcquireSpinLock(&Extension->ControlLock, &oldIrql);
    {
        PLIST_ENTRY entry;

        entry = Extension->PendingEvents.Flink;
        while (entry != &Extension->PendingEvents) {
            PLIST_ENTRY nextEntry;
            PYUMEDISK_EVENT_NODE eventNode;

            nextEntry = entry->Flink;
            eventNode = CONTAINING_RECORD(entry, YUMEDISK_EVENT_NODE, Link);
            if (eventNode->PendingIo != NULL) {
                RemoveEntryList(entry);
                Extension->PendingEventCount--;
                DiskFree(eventNode);
            }

            entry = nextEntry;
        }
    }
    KeReleaseSpinLock(&Extension->ControlLock, oldIrql);

    for (;;) {
        PYUMEDISK_PENDING_IO_NODE pendingIo;

        KeAcquireSpinLock(&Extension->ControlLock, &oldIrql);
        if (IsListEmpty(&Extension->PendingIo)) {
            KeReleaseSpinLock(&Extension->ControlLock, oldIrql);
            break;
        }

        pendingIo = CONTAINING_RECORD(RemoveHeadList(&Extension->PendingIo), YUMEDISK_PENDING_IO_NODE, Link);
        Extension->PendingIoCount--;
        KeReleaseSpinLock(&Extension->ControlLock, oldIrql);

        DiskCompleteScsiSrb(DeviceExtension, pendingIo, Status, NULL, 0);
    }
}

NTSTATUS
DiskQueuePendingScsiIo(
    _In_ PVOID DeviceExtension,
    _Inout_ PDEVICE_CONTEXT Extension,
    _In_ PSTORAGE_REQUEST_BLOCK Srb,
    _In_ ULONG Type,
    _In_ ULONG TargetId,
    _In_ ULONGLONG Lba,
    _In_ ULONG BlockCount,
    _In_ ULONG DataLength
)
{
    PYUMEDISK_PENDING_IO_NODE pendingIo;
    PYUMEDISK_EVENT_NODE eventNode;
    KIRQL oldIrql;

    if (Srb == NULL || (DataLength != 0 && Srb->DataBuffer == NULL)) {
        return STATUS_INVALID_PARAMETER;
    }

    pendingIo = (PYUMEDISK_PENDING_IO_NODE)DiskAlloc(sizeof(YUMEDISK_PENDING_IO_NODE));
    eventNode = (PYUMEDISK_EVENT_NODE)DiskAlloc(sizeof(YUMEDISK_EVENT_NODE));
    if (pendingIo == NULL || eventNode == NULL) {
        if (pendingIo != NULL) {
            DiskFree(pendingIo);
        }
        if (eventNode != NULL) {
            DiskFree(eventNode);
        }
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    RtlZeroMemory(pendingIo, sizeof(*pendingIo));
    RtlZeroMemory(eventNode, sizeof(*eventNode));

    pendingIo->Srb = Srb;
    pendingIo->Type = Type;
    pendingIo->TargetId = TargetId;
    pendingIo->Lba = Lba;
    pendingIo->BlockCount = BlockCount;
    pendingIo->DataLength = DataLength;

    eventNode->PendingIo = pendingIo;
    eventNode->Event.EventType = (Type == DiskPendingIoRead) ? YumeDiskEventReadRequest : YumeDiskEventWriteRequest;
    eventNode->Event.TargetId = TargetId;
    eventNode->Event.Lba = Lba;
    eventNode->Event.BlockCount = BlockCount;
    eventNode->Event.DataLength = DataLength;
    if (Type == DiskPendingIoWrite && DataLength != 0) {
        eventNode->Event.EventFlags |= YumeDiskEventFlagHasInlineData;
    }

    KeAcquireSpinLock(&Extension->ControlLock, &oldIrql);
    if (Extension->CurrentSessionId == 0) {
        KeReleaseSpinLock(&Extension->ControlLock, oldIrql);
        DiskFree(eventNode);
        DiskFree(pendingIo);
        return STATUS_DEVICE_NOT_CONNECTED;
    }

    pendingIo->TxId = DiskAllocateTxIdLocked(Extension);
    eventNode->Event.TxId = pendingIo->TxId;
    InsertTailList(&Extension->PendingIo, &pendingIo->Link);
    Extension->PendingIoCount++;
    KeReleaseSpinLock(&Extension->ControlLock, oldIrql);

    DiskQueueOrDeliverEventNode(DeviceExtension, eventNode);
    return STATUS_PENDING;
}

VOID
DiskFreeQueuedState(
    _In_ PVOID DeviceExtension
)
{
    PDEVICE_CONTEXT extension;
    KIRQL oldIrql;

    extension = (PDEVICE_CONTEXT)DeviceExtension;

    for (;;) {
        PLIST_ENTRY entry;
        PYUMEDISK_EVENT_NODE eventNode;

        KeAcquireSpinLock(&extension->ControlLock, &oldIrql);
        if (IsListEmpty(&extension->PendingEvents)) {
            KeReleaseSpinLock(&extension->ControlLock, oldIrql);
            break;
        }

        entry = RemoveHeadList(&extension->PendingEvents);
        extension->PendingEventCount--;
        KeReleaseSpinLock(&extension->ControlLock, oldIrql);

        eventNode = CONTAINING_RECORD(entry, YUMEDISK_EVENT_NODE, Link);
        DiskFree(eventNode);
    }
}

VOID
DiskCompleteAllPending(
    _In_ PVOID DeviceExtension,
    _In_ NTSTATUS Status
)
{
    PDEVICE_CONTEXT extension;
    KIRQL oldIrql;

    extension = (PDEVICE_CONTEXT)DeviceExtension;

    for (;;) {
        PLIST_ENTRY entry;
        PYUMEDISK_WAITER_NODE waiterNode;

        KeAcquireSpinLock(&extension->ControlLock, &oldIrql);
        if (IsListEmpty(&extension->PendingWaiters)) {
            KeReleaseSpinLock(&extension->ControlLock, oldIrql);
            break;
        }

        entry = RemoveHeadList(&extension->PendingWaiters);
        extension->PendingWaiterCount--;
        KeReleaseSpinLock(&extension->ControlLock, oldIrql);

        waiterNode = CONTAINING_RECORD(entry, YUMEDISK_WAITER_NODE, Link);
        DiskCompleteWaiterWithStatus(DeviceExtension, waiterNode, Status);
    }

    DiskCancelAllPendingIo(DeviceExtension, extension, Status);
}

VOID
DiskQueueSyntheticEvent(
    _In_ PVOID DeviceExtension,
    _In_ ULONG EventType,
    _In_ ULONG TargetId
)
{
    PDEVICE_CONTEXT extension;
    PYUMEDISK_EVENT_NODE eventNode;
    KIRQL oldIrql;
    BOOLEAN allowWhenSessionClosed;

    extension = (PDEVICE_CONTEXT)DeviceExtension;
    allowWhenSessionClosed = (EventType == YumeDiskEventShutdown);

    eventNode = (PYUMEDISK_EVENT_NODE)DiskAlloc(sizeof(YUMEDISK_EVENT_NODE));
    if (eventNode == NULL) {
        return;
    }

    RtlZeroMemory(eventNode, sizeof(*eventNode));
    eventNode->Event.EventType = EventType;
    eventNode->Event.TargetId = TargetId;

    KeAcquireSpinLock(&extension->ControlLock, &oldIrql);
    if (!allowWhenSessionClosed && extension->CurrentSessionId == 0) {
        KeReleaseSpinLock(&extension->ControlLock, oldIrql);
        DiskFree(eventNode);
        return;
    }

    eventNode->Event.TxId = DiskAllocateTxIdLocked(extension);
    KeReleaseSpinLock(&extension->ControlLock, oldIrql);
    DiskQueueOrDeliverEventNode(DeviceExtension, eventNode);
}

NTSTATUS
DiskHandleReadReply(
    _In_ PVOID DeviceExtension,
    _Inout_ PDEVICE_CONTEXT Extension,
    _Inout_ PYUMEDISK_MESSAGE Message
)
{
    PYUMEDISK_READ_REPLY reply;
    PYUMEDISK_PENDING_IO_NODE pendingIo;
    KIRQL oldIrql;
    NTSTATUS completionStatus;
    ULONG inlineDataLength;

    if (Message->Header.PayloadLength < (ULONG)YUMEDISK_READ_REPLY_BASE_SIZE) {
        return STATUS_INVALID_PARAMETER;
    }

    reply = (PYUMEDISK_READ_REPLY)Message->Payload;
    inlineDataLength = Message->Header.PayloadLength - (ULONG)YUMEDISK_READ_REPLY_BASE_SIZE;
    if (reply->TxId == 0 || reply->DataLength != inlineDataLength) {
        return STATUS_INVALID_PARAMETER;
    }

    KeAcquireSpinLock(&Extension->ControlLock, &oldIrql);
    pendingIo = DiskDetachPendingIoByTxIdLocked(Extension, reply->TxId);
    if (pendingIo != NULL) {
        DiskRemoveQueuedEventsForPendingIoLocked(Extension, pendingIo);
    }
    KeReleaseSpinLock(&Extension->ControlLock, oldIrql);

    if (pendingIo == NULL) {
        return STATUS_NOT_FOUND;
    }

    completionStatus = reply->IoStatus;
    if (pendingIo->Type != DiskPendingIoRead) {
        completionStatus = STATUS_INVALID_DEVICE_REQUEST;
    } else if (NT_SUCCESS(completionStatus) && reply->DataLength != pendingIo->DataLength) {
        completionStatus = STATUS_INVALID_BUFFER_SIZE;
    }

    DiskCompleteScsiSrb(
        DeviceExtension,
        pendingIo,
        completionStatus,
        reply->Data,
        NT_SUCCESS(completionStatus) ? reply->DataLength : 0);

    DiskInitMessageStatus(Message, YumeDiskCommandReadReply, completionStatus, 0);
    return completionStatus;
}

NTSTATUS
DiskHandleWriteAck(
    _In_ PVOID DeviceExtension,
    _Inout_ PDEVICE_CONTEXT Extension,
    _Inout_ PYUMEDISK_MESSAGE Message
)
{
    PYUMEDISK_WRITE_ACK ack;
    PYUMEDISK_PENDING_IO_NODE pendingIo;
    KIRQL oldIrql;
    NTSTATUS completionStatus;

    if (Message->Header.PayloadLength < sizeof(YUMEDISK_WRITE_ACK)) {
        return STATUS_INVALID_PARAMETER;
    }

    ack = (PYUMEDISK_WRITE_ACK)Message->Payload;
    if (ack->TxId == 0) {
        return STATUS_INVALID_PARAMETER;
    }

    KeAcquireSpinLock(&Extension->ControlLock, &oldIrql);
    pendingIo = DiskDetachPendingIoByTxIdLocked(Extension, ack->TxId);
    if (pendingIo != NULL) {
        DiskRemoveQueuedEventsForPendingIoLocked(Extension, pendingIo);
    }
    KeReleaseSpinLock(&Extension->ControlLock, oldIrql);

    if (pendingIo == NULL) {
        return STATUS_NOT_FOUND;
    }

    completionStatus = ack->IoStatus;
    if (pendingIo->Type != DiskPendingIoWrite) {
        completionStatus = STATUS_INVALID_DEVICE_REQUEST;
    }

    DiskCompleteScsiSrb(
        DeviceExtension,
        pendingIo,
        completionStatus,
        NULL,
        NT_SUCCESS(completionStatus) ? pendingIo->DataLength : 0);

    DiskInitMessageStatus(Message, YumeDiskCommandWriteAck, completionStatus, 0);
    return completionStatus;
}

NTSTATUS
DiskHandleWaitEvent(
    _In_ PVOID DeviceExtension,
    _Inout_ PDEVICE_CONTEXT Extension,
    _Inout_ PYUMEDISK_MESSAGE Message,
    _In_ PSTORAGE_REQUEST_BLOCK Srb
)
{
    KIRQL oldIrql;
    PYUMEDISK_EVENT_NODE eventNode;
    PYUMEDISK_WAITER_NODE waiterNode;
    PYUMEDISK_WAIT_EVENT waitRequest;
    NTSTATUS status;

    UNREFERENCED_PARAMETER(DeviceExtension);

    if (Message->Header.PayloadLength < sizeof(YUMEDISK_WAIT_EVENT) ||
        Message->Header.Size < YUMEDISK_MESSAGE_BASE_SIZE + sizeof(YUMEDISK_EVENT)) {
        return STATUS_BUFFER_TOO_SMALL;
    }

    waitRequest = (PYUMEDISK_WAIT_EVENT)Message->Payload;
    eventNode = NULL;
    waiterNode = NULL;
    status = STATUS_SUCCESS;

    KeAcquireSpinLock(&Extension->ControlLock, &oldIrql);
    if (Extension->CurrentSessionId == 0) {
        status = STATUS_DEVICE_NOT_CONNECTED;
    } else if (!IsListEmpty(&Extension->PendingEvents)) {
        PLIST_ENTRY entry;
        PYUMEDISK_EVENT_NODE candidate;

        entry = Extension->PendingEvents.Flink;
        candidate = CONTAINING_RECORD(entry, YUMEDISK_EVENT_NODE, Link);
        if (Message->Header.Size < (ULONG)(YUMEDISK_MESSAGE_BASE_SIZE + DiskGetEventPayloadLength(candidate))) {
            status = STATUS_BUFFER_TOO_SMALL;
        } else {
            entry = RemoveHeadList(&Extension->PendingEvents);
            Extension->PendingEventCount--;
            eventNode = CONTAINING_RECORD(entry, YUMEDISK_EVENT_NODE, Link);
        }
    } else if (waitRequest->TimeoutMs != 0) {
        status = STATUS_TIMEOUT;
    } else {
        waiterNode = (PYUMEDISK_WAITER_NODE)DiskAlloc(sizeof(YUMEDISK_WAITER_NODE));
        if (waiterNode != NULL) {
            waiterNode->Srb = Srb;
            InsertTailList(&Extension->PendingWaiters, &waiterNode->Link);
            Extension->PendingWaiterCount++;
        }
    }
    KeReleaseSpinLock(&Extension->ControlLock, oldIrql);

    if (eventNode != NULL) {
        status = DiskFillWaitEventMessage(Message, Message->Header.Size, eventNode);
        DiskFree(eventNode);
        return status;
    }

    if (!NT_SUCCESS(status)) {
        return status;
    }

    if (waiterNode == NULL) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    return STATUS_PENDING;
}

