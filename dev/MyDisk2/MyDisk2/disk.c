#include "disk.h"

#include <ntddscsi.h>

#include "define.h"
#include "utils.h"

#pragma warning(disable: 4100)

static
BOOLEAN
YumeDiskIsTargetVisible(
    _In_ PDEVICE_CONTEXT Extension,
    _In_ UCHAR TargetId
)
{
    if (TargetId >= Extension->MaxTargets) {
        return FALSE;
    }

    return Extension->Disk[TargetId].Configured && Extension->Disk[TargetId].Present;
}

static
VOID
YumeDiskInitMessageStatus(
    _Inout_ PYUMEDISK_MESSAGE Message,
    _In_ ULONG Command,
    _In_ NTSTATUS Status,
    _In_ ULONG PayloadLength
)
{
    Message->Header.Version = YUMEDISK_PROTOCOL_VERSION;
    Message->Header.Command = Command;
    Message->Header.Status = Status;
    Message->Header.PayloadLength = PayloadLength;
    Message->Header.Size = YUMEDISK_MESSAGE_BASE_SIZE + PayloadLength;
}

static
VOID
YumeDiskCompleteIoctlSrb(
    _In_ PSTORAGE_REQUEST_BLOCK Srb,
    _In_ PSRB_IO_CONTROL SrbIoControl,
    _In_ NTSTATUS Status,
    _In_ ULONG ResponseLength
)
{
    SrbIoControl->ReturnCode = (ULONG)Status;
    SrbIoControl->Length = ResponseLength;
    Srb->DataTransferLength = sizeof(SRB_IO_CONTROL) + ResponseLength;
    Srb->SrbStatus = SRB_STATUS_SUCCESS;
}

static
ULONGLONG
YumeDiskAllocateTxIdLocked(
    _Inout_ PDEVICE_CONTEXT Extension
)
{
    return ++Extension->NextTxId;
}

static
BOOLEAN
YumeDiskEventHasInlineData(
    _In_ const YUMEDISK_EVENT_NODE* EventNode
)
{
    return EventNode->Event.EventType == YumeDiskEventWriteRequest &&
        EventNode->Event.DataLength != 0;
}

static
ULONG
YumeDiskGetEventPayloadLength(
    _In_ const YUMEDISK_EVENT_NODE* EventNode
)
{
    ULONG payloadLength;

    payloadLength = sizeof(YUMEDISK_EVENT);
    if (YumeDiskEventHasInlineData(EventNode)) {
        payloadLength += EventNode->Event.DataLength;
    }

    return payloadLength;
}

static
NTSTATUS
YumeDiskFillWaitEventMessage(
    _Inout_ PYUMEDISK_MESSAGE Message,
    _In_ ULONG MessageCapacity,
    _In_ const YUMEDISK_EVENT_NODE* EventNode
)
{
    ULONG payloadLength;

    payloadLength = YumeDiskGetEventPayloadLength(EventNode);
    if (MessageCapacity < (ULONG)(YUMEDISK_MESSAGE_BASE_SIZE + payloadLength)) {
        return STATUS_BUFFER_TOO_SMALL;
    }

    YumeDiskInitMessageStatus(Message, YumeDiskCommandWaitEvent, STATUS_SUCCESS, payloadLength);
    RtlCopyMemory(Message->Payload, &EventNode->Event, sizeof(YUMEDISK_EVENT));

    if (YumeDiskEventHasInlineData(EventNode)) {
        PYUMEDISK_PENDING_IO_NODE pendingIo;

        pendingIo = EventNode->PendingIo;
        if (pendingIo == NULL || pendingIo->Srb == NULL || pendingIo->Srb->DataBuffer == NULL) {
            return STATUS_DEVICE_DATA_ERROR;
        }

        RtlCopyMemory(
            Message->Payload + sizeof(YUMEDISK_EVENT),
            pendingIo->Srb->DataBuffer,
            EventNode->Event.DataLength
        );
    }

    return STATUS_SUCCESS;
}

static
BOOLEAN
YumeDiskTryCompleteWaiterWithEvent(
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
        free(WaiterNode);
        return FALSE;
    }

    srbIoControl = (PSRB_IO_CONTROL)waiterSrb->DataBuffer;
    message = (PYUMEDISK_MESSAGE)(srbIoControl + 1);
    messageCapacity = srbIoControl->Length;
    status = YumeDiskFillWaitEventMessage(message, messageCapacity, EventNode);
    if (!NT_SUCCESS(status)) {
        YumeDiskInitMessageStatus(message, YumeDiskCommandWaitEvent, status, 0);
        YumeDiskCompleteIoctlSrb(waiterSrb, srbIoControl, status, message->Header.Size);
        StorPortNotification(RequestComplete, DeviceExtension, waiterSrb);
        free(WaiterNode);
        return FALSE;
    }

    YumeDiskCompleteIoctlSrb(waiterSrb, srbIoControl, STATUS_SUCCESS, message->Header.Size);
    StorPortNotification(RequestComplete, DeviceExtension, waiterSrb);
    free(WaiterNode);
    return TRUE;
}

static
VOID
YumeDiskCompleteWaiterWithStatus(
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
        free(WaiterNode);
        return;
    }

    srbIoControl = (PSRB_IO_CONTROL)waiterSrb->DataBuffer;
    message = (PYUMEDISK_MESSAGE)(srbIoControl + 1);
    YumeDiskInitMessageStatus(message, YumeDiskCommandWaitEvent, Status, 0);
    YumeDiskCompleteIoctlSrb(waiterSrb, srbIoControl, Status, message->Header.Size);
    StorPortNotification(RequestComplete, DeviceExtension, waiterSrb);
    free(WaiterNode);
}

static
VOID
YumeDiskQueueOrDeliverEventNode(
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
        if (YumeDiskTryCompleteWaiterWithEvent(DeviceExtension, waiterNode, EventNode)) {
            free(EventNode);
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
YumeDiskAssignTxId(
    _In_ PDEVICE_CONTEXT Extension,
    _Inout_ YUMEDISK_EVENT* Event
)
{
    KIRQL oldIrql;

    KeAcquireSpinLock(&Extension->ControlLock, &oldIrql);
    Event->TxId = YumeDiskAllocateTxIdLocked(Extension);
    KeReleaseSpinLock(&Extension->ControlLock, oldIrql);
}

VOID
YumeDiskResetDiskStorage(
    _Inout_ PYUME_DISK Disk
)
{
    KIRQL oldIrql;

    KeAcquireSpinLock(&Disk->BufferLock, &oldIrql);
    if (Disk->Buffer != NULL) {
        free(Disk->Buffer);
        Disk->Buffer = NULL;
    }
    KeReleaseSpinLock(&Disk->BufferLock, oldIrql);
}

static
NTSTATUS
YumeDiskClaimSessionLocked(
    _Inout_ PDEVICE_CONTEXT Extension,
    _In_ const YUMEDISK_HEADER* Header
)
{
    if (Header->Command == YumeDiskCommandQueryInfo) {
        return STATUS_SUCCESS;
    }

    if (Header->SessionId == 0) {
        return STATUS_INVALID_PARAMETER;
    }

    if (Extension->CurrentSessionId == 0) {
        Extension->CurrentSessionId = Header->SessionId;
        return STATUS_SUCCESS;
    }

    if (Extension->CurrentSessionId != Header->SessionId) {
        return STATUS_SHARING_VIOLATION;
    }

    return STATUS_SUCCESS;
}

static
PSRBEX_DATA_SCSI_CDB16
YumeDiskGetScsiCdb16Data(
    _In_ PSTORAGE_REQUEST_BLOCK Srb
)
{
    PUCHAR base;
    ULONG offset;
    PSRBEX_DATA exData;

    if (Srb == NULL || Srb->NumSrbExData != 1 || Srb->SrbExDataOffset == NULL) {
        return NULL;
    }

    offset = Srb->SrbExDataOffset[0];
    if (offset == 0 || offset >= Srb->SrbLength) {
        return NULL;
    }

    base = (PUCHAR)Srb;
    exData = (PSRBEX_DATA)(base + offset);
    if (exData->Type != SrbExDataTypeScsiCdb16) {
        return NULL;
    }

    return (PSRBEX_DATA_SCSI_CDB16)exData;
}

static
UCHAR
YumeDiskNtStatusToSrbStatus(
    _In_ NTSTATUS Status
)
{
    if (NT_SUCCESS(Status)) {
        return SRB_STATUS_SUCCESS;
    }

    switch (Status) {
    case STATUS_BUFFER_TOO_SMALL:
        return SRB_STATUS_DATA_OVERRUN;
    case STATUS_INVALID_PARAMETER:
    case STATUS_INVALID_DEVICE_REQUEST:
        return SRB_STATUS_INVALID_REQUEST;
    case STATUS_CANCELLED:
    case STATUS_TIMEOUT:
        return SRB_STATUS_ABORTED;
    case STATUS_DEVICE_DOES_NOT_EXIST:
    case STATUS_DEVICE_NOT_CONNECTED:
    case STATUS_NOT_FOUND:
        return SRB_STATUS_INVALID_TARGET_ID;
    default:
        return SRB_STATUS_ERROR;
    }
}

static
VOID
YumeDiskCompleteScsiSrb(
    _In_ PVOID DeviceExtension,
    _In_ PYUMEDISK_PENDING_IO_NODE PendingIo,
    _In_ NTSTATUS Status,
    _In_reads_bytes_opt_(DataLength) const VOID* Data,
    _In_ ULONG DataLength
)
{
    PSTORAGE_REQUEST_BLOCK srb;
    PSRBEX_DATA_SCSI_CDB16 cdb16;

    srb = PendingIo->Srb;
    if (srb == NULL) {
        free(PendingIo);
        return;
    }

    cdb16 = YumeDiskGetScsiCdb16Data(srb);
    if (NT_SUCCESS(Status)) {
        if (PendingIo->Type == YumeDiskPendingIoRead && DataLength != 0) {
            if (srb->DataBuffer == NULL || Data == NULL) {
                Status = STATUS_INVALID_PARAMETER;
            } else {
                RtlCopyMemory(srb->DataBuffer, Data, DataLength);
            }
        }
    }

    if (NT_SUCCESS(Status)) {
        srb->DataTransferLength = DataLength;
        srb->SrbStatus = SRB_STATUS_SUCCESS;
        if (cdb16 != NULL) {
            cdb16->ScsiStatus = SCSISTAT_GOOD;
        }
    } else {
        srb->DataTransferLength = 0;
        srb->SrbStatus = YumeDiskNtStatusToSrbStatus(Status);
        if (cdb16 != NULL) {
            cdb16->ScsiStatus = SCSISTAT_GOOD;
        }
    }

    StorPortNotification(RequestComplete, DeviceExtension, srb);
    free(PendingIo);
}

static
VOID
YumeDiskRemoveQueuedEventsForPendingIoLocked(
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
            free(eventNode);
        }

        entry = nextEntry;
    }
}

static
VOID
YumeDiskRemoveQueuedIoEventsByTargetLocked(
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
            free(eventNode);
        }

        entry = nextEntry;
    }
}

static
PYUMEDISK_PENDING_IO_NODE
YumeDiskDetachPendingIoByTxIdLocked(
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
YumeDiskDetachPendingIoByTargetLocked(
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

static
VOID
YumeDiskCancelPendingIoByTarget(
    _In_ PVOID DeviceExtension,
    _Inout_ PDEVICE_CONTEXT Extension,
    _In_ ULONG TargetId,
    _In_ NTSTATUS Status
)
{
    KIRQL oldIrql;

    KeAcquireSpinLock(&Extension->ControlLock, &oldIrql);
    YumeDiskRemoveQueuedIoEventsByTargetLocked(Extension, TargetId);
    KeReleaseSpinLock(&Extension->ControlLock, oldIrql);

    for (;;) {
        PYUMEDISK_PENDING_IO_NODE pendingIo;

        KeAcquireSpinLock(&Extension->ControlLock, &oldIrql);
        pendingIo = YumeDiskDetachPendingIoByTargetLocked(Extension, TargetId);
        KeReleaseSpinLock(&Extension->ControlLock, oldIrql);
        if (pendingIo == NULL) {
            break;
        }

        YumeDiskCompleteScsiSrb(DeviceExtension, pendingIo, Status, NULL, 0);
    }
}

static
VOID
YumeDiskCancelAllPendingIo(
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
                free(eventNode);
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

        YumeDiskCompleteScsiSrb(DeviceExtension, pendingIo, Status, NULL, 0);
    }
}

static
NTSTATUS
YumeDiskQueuePendingScsiIo(
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

    pendingIo = (PYUMEDISK_PENDING_IO_NODE)malloc(sizeof(YUMEDISK_PENDING_IO_NODE));
    eventNode = (PYUMEDISK_EVENT_NODE)malloc(sizeof(YUMEDISK_EVENT_NODE));
    if (pendingIo == NULL || eventNode == NULL) {
        if (pendingIo != NULL) {
            free(pendingIo);
        }
        if (eventNode != NULL) {
            free(eventNode);
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
    eventNode->Event.EventType = (Type == YumeDiskPendingIoRead) ? YumeDiskEventReadRequest : YumeDiskEventWriteRequest;
    eventNode->Event.TargetId = TargetId;
    eventNode->Event.Lba = Lba;
    eventNode->Event.BlockCount = BlockCount;
    eventNode->Event.DataLength = DataLength;
    if (Type == YumeDiskPendingIoWrite && DataLength != 0) {
        eventNode->Event.EventFlags |= YumeDiskEventFlagHasInlineData;
    }

    KeAcquireSpinLock(&Extension->ControlLock, &oldIrql);
    if (Extension->CurrentSessionId == 0) {
        KeReleaseSpinLock(&Extension->ControlLock, oldIrql);
        free(eventNode);
        free(pendingIo);
        return STATUS_DEVICE_NOT_CONNECTED;
    }

    pendingIo->TxId = YumeDiskAllocateTxIdLocked(Extension);
    eventNode->Event.TxId = pendingIo->TxId;
    InsertTailList(&Extension->PendingIo, &pendingIo->Link);
    Extension->PendingIoCount++;
    KeReleaseSpinLock(&Extension->ControlLock, oldIrql);

    YumeDiskQueueOrDeliverEventNode(DeviceExtension, eventNode);
    return STATUS_PENDING;
}

VOID
YumeDiskFreeQueuedState(
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
        free(eventNode);
    }
}

VOID
YumeDiskCompleteAllPending(
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
        YumeDiskCompleteWaiterWithStatus(DeviceExtension, waiterNode, Status);
    }

    YumeDiskCancelAllPendingIo(DeviceExtension, extension, Status);
}

VOID
YumeDiskQueueSyntheticEvent(
    _In_ PVOID DeviceExtension,
    _In_ ULONG EventType,
    _In_ ULONG TargetId
)
{
    PDEVICE_CONTEXT extension;
    PYUMEDISK_EVENT_NODE eventNode;

    extension = (PDEVICE_CONTEXT)DeviceExtension;
    if (extension->CurrentSessionId == 0) {
        return;
    }

    eventNode = (PYUMEDISK_EVENT_NODE)malloc(sizeof(YUMEDISK_EVENT_NODE));
    if (eventNode == NULL) {
        return;
    }

    RtlZeroMemory(eventNode, sizeof(*eventNode));
    eventNode->Event.EventType = EventType;
    eventNode->Event.TargetId = TargetId;
    YumeDiskAssignTxId(extension, &eventNode->Event);
    YumeDiskQueueOrDeliverEventNode(DeviceExtension, eventNode);
}

static
BOOLEAN
YumeDiskValidateDiskRange(
    _In_ PYUME_DISK Disk,
    _In_ UINT64 StartBlockIndex,
    _In_ ULONG BlockCount,
    _In_ ULONG TransferLength,
    _Out_ UINT64* StartByte,
    _Out_ UINT64* ByteCount
)
{
    UINT64 endBlockIndex;

    if (Disk->SectorSize == 0) {
        return FALSE;
    }

    if (BlockCount == 0) {
        *StartByte = 0;
        *ByteCount = 0;
        return TRUE;
    }

    endBlockIndex = StartBlockIndex + BlockCount;
    if (endBlockIndex < StartBlockIndex) {
        return FALSE;
    }

    *StartByte = StartBlockIndex * Disk->SectorSize;
    *ByteCount = (UINT64)BlockCount * Disk->SectorSize;

    if (*StartByte > Disk->Size || *ByteCount > Disk->Size || (*StartByte + *ByteCount) > Disk->Size) {
        return FALSE;
    }

    if (*ByteCount > TransferLength) {
        return FALSE;
    }

    return TRUE;
}

static
NTSTATUS
YumeDiskHandleQueryInfo(
    _In_ PDEVICE_CONTEXT Extension,
    _Inout_ PYUMEDISK_MESSAGE Message
)
{
    PYUMEDISK_QUERY_INFO info;

    if (Message->Header.Size < YUMEDISK_MESSAGE_BASE_SIZE + sizeof(YUMEDISK_QUERY_INFO)) {
        return STATUS_BUFFER_TOO_SMALL;
    }

    info = (PYUMEDISK_QUERY_INFO)Message->Payload;
    RtlZeroMemory(info, sizeof(*info));
    info->ProtocolVersion = YUMEDISK_PROTOCOL_VERSION;
    info->MaxTargets = Extension->MaxTargets;
    info->Features = YumeDiskFeatureWaitEvent | YumeDiskFeatureDynamicDisk | YumeDiskFeatureIoSkeleton;
    RtlCopyMemory(info->AdapterSignature, YUMEDISK_MINIPORT_SIGNATURE, sizeof(info->AdapterSignature));
    RtlCopyMemory(info->ServiceName, L"MyDisk2", sizeof(L"MyDisk2"));

    YumeDiskInitMessageStatus(Message, YumeDiskCommandQueryInfo, STATUS_SUCCESS, sizeof(YUMEDISK_QUERY_INFO));
    if (Extension->CurrentSessionId != 0) {
        Message->Header.SessionId = Extension->CurrentSessionId;
    }
    return STATUS_SUCCESS;
}

static
NTSTATUS
YumeDiskHandleCreateDisk(
    _In_ PVOID DeviceExtension,
    _Inout_ PDEVICE_CONTEXT Extension,
    _Inout_ PYUMEDISK_MESSAGE Message
)
{
    PYUMEDISK_CREATE_DISK request;
    PYUME_DISK disk;
    UINT64 diskSize;

    if (Message->Header.PayloadLength < sizeof(YUMEDISK_CREATE_DISK)) {
        return STATUS_INVALID_PARAMETER;
    }

    request = (PYUMEDISK_CREATE_DISK)Message->Payload;
    if (request->TargetId >= Extension->MaxTargets ||
        request->SectorSize == 0 ||
        request->SectorCount == 0 ||
        request->SectorCount > (MAXULONGLONG / request->SectorSize)) {
        return STATUS_INVALID_PARAMETER;
    }

    disk = &Extension->Disk[request->TargetId];
    if (disk->Present) {
        return STATUS_OBJECT_NAME_COLLISION;
    }

    YumeDiskResetDiskStorage(disk);
    diskSize = request->SectorCount * request->SectorSize;
    disk->SectorSize = request->SectorSize;
    disk->SectorCount = request->SectorCount;
    disk->Size = diskSize;
    disk->Configured = TRUE;
    disk->Present = TRUE;
    disk->Removing = FALSE;
    disk->Generation++;

    YumeDiskInitMessageStatus(Message, YumeDiskCommandCreateDisk, STATUS_SUCCESS, 0);
    YumeDiskQueueSyntheticEvent(DeviceExtension, YumeDiskEventDiskAdded, request->TargetId);
    StorPortNotification(BusChangeDetected, DeviceExtension, 0);
    return STATUS_SUCCESS;
}

static
NTSTATUS
YumeDiskHandleRemoveDisk(
    _In_ PVOID DeviceExtension,
    _Inout_ PDEVICE_CONTEXT Extension,
    _Inout_ PYUMEDISK_MESSAGE Message
)
{
    PYUMEDISK_REMOVE_DISK request;
    PYUME_DISK disk;

    if (Message->Header.PayloadLength < sizeof(YUMEDISK_REMOVE_DISK)) {
        return STATUS_INVALID_PARAMETER;
    }

    request = (PYUMEDISK_REMOVE_DISK)Message->Payload;
    if (request->TargetId >= Extension->MaxTargets) {
        return STATUS_INVALID_PARAMETER;
    }

    disk = &Extension->Disk[request->TargetId];
    if (!disk->Present) {
        return STATUS_NOT_FOUND;
    }

    disk->Configured = FALSE;
    disk->Present = FALSE;
    disk->Removing = TRUE;
    YumeDiskCancelPendingIoByTarget(DeviceExtension, Extension, request->TargetId, STATUS_DEVICE_NOT_CONNECTED);
    YumeDiskResetDiskStorage(disk);
    disk->Generation++;

    YumeDiskInitMessageStatus(Message, YumeDiskCommandRemoveDisk, STATUS_SUCCESS, 0);
    YumeDiskQueueSyntheticEvent(DeviceExtension, YumeDiskEventDiskRemoved, request->TargetId);
    StorPortNotification(BusChangeDetected, DeviceExtension, 0);
    return STATUS_SUCCESS;
}

static
NTSTATUS
YumeDiskHandleRemoveAllDisks(
    _In_ PVOID DeviceExtension,
    _Inout_ PDEVICE_CONTEXT Extension,
    _Inout_ PYUMEDISK_MESSAGE Message
)
{
    ULONG index;

    for (index = 0; index < Extension->MaxTargets; ++index) {
        if (Extension->Disk[index].Present) {
            Extension->Disk[index].Configured = FALSE;
            Extension->Disk[index].Present = FALSE;
            Extension->Disk[index].Removing = TRUE;
            YumeDiskCancelPendingIoByTarget(DeviceExtension, Extension, index, STATUS_DEVICE_NOT_CONNECTED);
            YumeDiskResetDiskStorage(&Extension->Disk[index]);
            Extension->Disk[index].Generation++;
            YumeDiskQueueSyntheticEvent(DeviceExtension, YumeDiskEventDiskRemoved, index);
        }
    }

    if ((Message->Header.Flags & YUMEDISK_SESSION_CLOSE_FLAG) != 0) {
        YumeDiskQueueSyntheticEvent(DeviceExtension, YumeDiskEventShutdown, 0);
        Extension->CurrentSessionId = 0;
        YumeDiskFreeQueuedState(DeviceExtension);
        YumeDiskCompleteAllPending(DeviceExtension, STATUS_DEVICE_NOT_CONNECTED);
    }

    YumeDiskInitMessageStatus(Message, YumeDiskCommandRemoveAllDisks, STATUS_SUCCESS, 0);
    StorPortNotification(BusChangeDetected, DeviceExtension, 0);
    return STATUS_SUCCESS;
}

static
NTSTATUS
YumeDiskHandleReadReply(
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
    pendingIo = YumeDiskDetachPendingIoByTxIdLocked(Extension, reply->TxId);
    if (pendingIo != NULL) {
        YumeDiskRemoveQueuedEventsForPendingIoLocked(Extension, pendingIo);
    }
    KeReleaseSpinLock(&Extension->ControlLock, oldIrql);

    if (pendingIo == NULL) {
        return STATUS_NOT_FOUND;
    }

    completionStatus = reply->IoStatus;
    if (pendingIo->Type != YumeDiskPendingIoRead) {
        completionStatus = STATUS_INVALID_DEVICE_REQUEST;
    } else if (NT_SUCCESS(completionStatus) && reply->DataLength != pendingIo->DataLength) {
        completionStatus = STATUS_INVALID_BUFFER_SIZE;
    }

    YumeDiskCompleteScsiSrb(
        DeviceExtension,
        pendingIo,
        completionStatus,
        reply->Data,
        NT_SUCCESS(completionStatus) ? reply->DataLength : 0
    );

    YumeDiskInitMessageStatus(Message, YumeDiskCommandReadReply, completionStatus, 0);
    return completionStatus;
}

static
NTSTATUS
YumeDiskHandleWriteAck(
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
    pendingIo = YumeDiskDetachPendingIoByTxIdLocked(Extension, ack->TxId);
    if (pendingIo != NULL) {
        YumeDiskRemoveQueuedEventsForPendingIoLocked(Extension, pendingIo);
    }
    KeReleaseSpinLock(&Extension->ControlLock, oldIrql);

    if (pendingIo == NULL) {
        return STATUS_NOT_FOUND;
    }

    completionStatus = ack->IoStatus;
    if (pendingIo->Type != YumeDiskPendingIoWrite) {
        completionStatus = STATUS_INVALID_DEVICE_REQUEST;
    }

    YumeDiskCompleteScsiSrb(
        DeviceExtension,
        pendingIo,
        completionStatus,
        NULL,
        NT_SUCCESS(completionStatus) ? pendingIo->DataLength : 0
    );

    YumeDiskInitMessageStatus(Message, YumeDiskCommandWriteAck, completionStatus, 0);
    return completionStatus;
}

static
NTSTATUS
YumeDiskHandleWaitEvent(
    _In_ PVOID DeviceExtension,
    _Inout_ PDEVICE_CONTEXT Extension,
    _Inout_ PYUMEDISK_MESSAGE Message,
    _In_ PSTORAGE_REQUEST_BLOCK Srb
)
{
    KIRQL oldIrql;
    PYUMEDISK_EVENT_NODE eventNode;
    PYUMEDISK_WAITER_NODE waiterNode;
    NTSTATUS status;

    UNREFERENCED_PARAMETER(DeviceExtension);

    if (Message->Header.PayloadLength < sizeof(YUMEDISK_WAIT_EVENT) ||
        Message->Header.Size < YUMEDISK_MESSAGE_BASE_SIZE + sizeof(YUMEDISK_EVENT)) {
        return STATUS_BUFFER_TOO_SMALL;
    }

    eventNode = NULL;
    waiterNode = NULL;
    status = STATUS_SUCCESS;

    KeAcquireSpinLock(&Extension->ControlLock, &oldIrql);
    if (!IsListEmpty(&Extension->PendingEvents)) {
        PLIST_ENTRY entry;
        PYUMEDISK_EVENT_NODE candidate;

        entry = Extension->PendingEvents.Flink;
        candidate = CONTAINING_RECORD(entry, YUMEDISK_EVENT_NODE, Link);
        if (Message->Header.Size < (ULONG)(YUMEDISK_MESSAGE_BASE_SIZE + YumeDiskGetEventPayloadLength(candidate))) {
            status = STATUS_BUFFER_TOO_SMALL;
        } else {
            entry = RemoveHeadList(&Extension->PendingEvents);
            Extension->PendingEventCount--;
            eventNode = CONTAINING_RECORD(entry, YUMEDISK_EVENT_NODE, Link);
        }
    } else {
        waiterNode = (PYUMEDISK_WAITER_NODE)malloc(sizeof(YUMEDISK_WAITER_NODE));
        if (waiterNode != NULL) {
            waiterNode->Srb = Srb;
            InsertTailList(&Extension->PendingWaiters, &waiterNode->Link);
            Extension->PendingWaiterCount++;
        }
    }
    KeReleaseSpinLock(&Extension->ControlLock, oldIrql);

    if (eventNode != NULL) {
        status = YumeDiskFillWaitEventMessage(Message, Message->Header.Size, eventNode);
        free(eventNode);
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

BOOLEAN
YumeDiskHandleIoControlSrb(
    _In_ PVOID DeviceExtension,
    _In_ PSTORAGE_REQUEST_BLOCK Srb
)
{
    PDEVICE_CONTEXT extension;
    PSRB_IO_CONTROL srbIoControl;
    PYUMEDISK_MESSAGE message;
    NTSTATUS status;
    KIRQL oldIrql;

    extension = (PDEVICE_CONTEXT)DeviceExtension;
    status = STATUS_SUCCESS;

    if (Srb->DataBuffer == NULL || Srb->DataTransferLength < (ULONG)(sizeof(SRB_IO_CONTROL) + YUMEDISK_MESSAGE_BASE_SIZE)) {
        Srb->SrbStatus = SRB_STATUS_INVALID_REQUEST;
        StorPortNotification(RequestComplete, DeviceExtension, Srb);
        return TRUE;
    }

    srbIoControl = (PSRB_IO_CONTROL)Srb->DataBuffer;
    if (srbIoControl->HeaderLength != sizeof(SRB_IO_CONTROL) ||
        RtlCompareMemory(srbIoControl->Signature, YUMEDISK_MINIPORT_SIGNATURE, sizeof(srbIoControl->Signature)) != sizeof(srbIoControl->Signature)) {
        Srb->SrbStatus = SRB_STATUS_INVALID_REQUEST;
        StorPortNotification(RequestComplete, DeviceExtension, Srb);
        return TRUE;
    }

    if (Srb->DataTransferLength < (ULONG)(sizeof(SRB_IO_CONTROL) + srbIoControl->Length) ||
        srbIoControl->Length < (ULONG)YUMEDISK_MESSAGE_BASE_SIZE) {
        Srb->SrbStatus = SRB_STATUS_INVALID_REQUEST;
        StorPortNotification(RequestComplete, DeviceExtension, Srb);
        return TRUE;
    }

    message = (PYUMEDISK_MESSAGE)(srbIoControl + 1);
    if (message->Header.Version != YUMEDISK_PROTOCOL_VERSION ||
        message->Header.Size < (ULONG)YUMEDISK_MESSAGE_BASE_SIZE ||
        message->Header.Size > srbIoControl->Length ||
        (ULONG)(YUMEDISK_MESSAGE_BASE_SIZE + message->Header.PayloadLength) > message->Header.Size) {
        YumeDiskInitMessageStatus(message, message->Header.Command, STATUS_REVISION_MISMATCH, 0);
        YumeDiskCompleteIoctlSrb(Srb, srbIoControl, STATUS_REVISION_MISMATCH, message->Header.Size);
        StorPortNotification(RequestComplete, DeviceExtension, Srb);
        return TRUE;
    }

    KeAcquireSpinLock(&extension->ControlLock, &oldIrql);
    status = YumeDiskClaimSessionLocked(extension, &message->Header);
    KeReleaseSpinLock(&extension->ControlLock, oldIrql);

    if (!NT_SUCCESS(status)) {
        YumeDiskInitMessageStatus(message, message->Header.Command, status, 0);
        YumeDiskCompleteIoctlSrb(Srb, srbIoControl, status, message->Header.Size);
        StorPortNotification(RequestComplete, DeviceExtension, Srb);
        return TRUE;
    }

    switch (message->Header.Command) {
    case YumeDiskCommandQueryInfo:
        status = YumeDiskHandleQueryInfo(extension, message);
        break;
    case YumeDiskCommandCreateDisk:
        status = YumeDiskHandleCreateDisk(DeviceExtension, extension, message);
        break;
    case YumeDiskCommandRemoveDisk:
        status = YumeDiskHandleRemoveDisk(DeviceExtension, extension, message);
        break;
    case YumeDiskCommandRemoveAllDisks:
        status = YumeDiskHandleRemoveAllDisks(DeviceExtension, extension, message);
        break;
    case YumeDiskCommandWaitEvent:
        status = YumeDiskHandleWaitEvent(DeviceExtension, extension, message, Srb);
        break;
    case YumeDiskCommandHeartbeat:
        YumeDiskInitMessageStatus(message, YumeDiskCommandHeartbeat, STATUS_SUCCESS, 0);
        status = STATUS_SUCCESS;
        break;
    case YumeDiskCommandReadReply:
        status = YumeDiskHandleReadReply(DeviceExtension, extension, message);
        break;
    case YumeDiskCommandWriteAck:
        status = YumeDiskHandleWriteAck(DeviceExtension, extension, message);
        break;
    default:
        YumeDiskInitMessageStatus(message, message->Header.Command, STATUS_INVALID_DEVICE_REQUEST, 0);
        status = STATUS_INVALID_DEVICE_REQUEST;
        break;
    }

    if (status == STATUS_PENDING) {
        Srb->SrbStatus = SRB_STATUS_PENDING;
        return TRUE;
    }

    if (!NT_SUCCESS(status) && message->Header.Status == STATUS_SUCCESS) {
        YumeDiskInitMessageStatus(message, message->Header.Command, status, 0);
    }

    YumeDiskCompleteIoctlSrb(Srb, srbIoControl, status, message->Header.Size);
    StorPortNotification(RequestComplete, DeviceExtension, Srb);
    return TRUE;
}

VOID
YumeDiskHandleScsiCdb(
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
)
{
    PDEVICE_CONTEXT extension;
    PYUME_DISK disk;
    ULONG transferLength;
    UINT64 startBlockIndex;
    UINT64 startByte;
    UINT64 byteCount;
    ULONG blockCount;
    NTSTATUS ioStatus;

    UNREFERENCED_PARAMETER(SenseInfoBuffer);
    UNREFERENCED_PARAMETER(SenseInfoBufferLength);

    extension = (PDEVICE_CONTEXT)DeviceExtension;
    transferLength = *DataTransferLength;
    *DataTransferLength = 0;
    *SrbStatus = SRB_STATUS_SUCCESS;
    *ScsiStatus = SCSISTAT_GOOD;

    if (extension == NULL) {
        *SrbStatus = SRB_STATUS_INVALID_REQUEST;
        return;
    }

    if (!YumeDiskIsTargetVisible(extension, TargetId)) {
        *SrbStatus = SRB_STATUS_INVALID_TARGET_ID;
        return;
    }

    disk = &extension->Disk[TargetId];
    startBlockIndex = 0;
    startByte = 0;
    byteCount = 0;
    blockCount = 0;

    switch (Cdb->CDB6GENERIC.OperationCode) {
    case SCSIOP_REPORT_LUNS:
    {
        REPORT_LUNS_DATA reportLuns;
        UINT32 lunListSize;
        UINT64 lun0;
        ULONG returnLength;

        RtlZeroMemory(&reportLuns, sizeof(reportLuns));
        lunListSize = sizeof(UINT64);
        lun0 = 0;
        returnLength = min(transferLength, (ULONG)sizeof(reportLuns));

        REVERSE_BYTES_4(&reportLuns.LunListLength, &lunListSize);
        REVERSE_BYTES_8(&reportLuns.Entry[0], &lun0);
        RtlCopyMemory(DataBuffer, &reportLuns, returnLength);
        *DataTransferLength = returnLength;

        if (transferLength < sizeof(reportLuns)) {
            *SrbStatus = SRB_STATUS_DATA_OVERRUN;
        }
        break;
    }
    case SCSIOP_INQUIRY:
    {
        PINQUIRYDATA inquiryData;

        if (transferLength < INQUIRYDATABUFFERSIZE) {
            *SrbStatus = SRB_STATUS_DATA_OVERRUN;
            break;
        }

        inquiryData = (PINQUIRYDATA)DataBuffer;
        RtlZeroMemory(inquiryData, INQUIRYDATABUFFERSIZE);
        inquiryData->DeviceType = DIRECT_ACCESS_DEVICE;
        inquiryData->DeviceTypeQualifier = DEVICE_QUALIFIER_ACTIVE;
        inquiryData->RemovableMedia = FALSE;
        inquiryData->CommandQueue = TRUE;
        inquiryData->Versions = 0x05;
        inquiryData->VersionDescriptors[0] = 0x0960;
        inquiryData->VersionDescriptors[1] = 0x0060;

        RtlCopyMemory(inquiryData->VendorId, "Yume    ", 8);
        RtlCopyMemory(inquiryData->ProductId, "YumeDisk        ", 16);
        RtlCopyMemory(inquiryData->ProductRevisionLevel, "1.0 ", 4);

        *DataTransferLength = INQUIRYDATABUFFERSIZE;
        break;
    }
    case SCSIOP_READ_CAPACITY:
    {
        PREAD_CAPACITY_DATA readCapacity;
        UINT64 maxLba64;
        UINT32 maxLba32;
        UINT32 sectorSize32;

        if (transferLength < sizeof(READ_CAPACITY_DATA)) {
            *SrbStatus = SRB_STATUS_DATA_OVERRUN;
            break;
        }

        readCapacity = (PREAD_CAPACITY_DATA)DataBuffer;
        maxLba64 = disk->SectorCount - 1;
        maxLba32 = (maxLba64 >= MAXULONG) ? MAXULONG : (UINT32)maxLba64;
        sectorSize32 = disk->SectorSize;

        REVERSE_BYTES_4(&readCapacity->LogicalBlockAddress, &maxLba32);
        REVERSE_BYTES_4(&readCapacity->BytesPerBlock, &sectorSize32);
        *DataTransferLength = sizeof(READ_CAPACITY_DATA);
        break;
    }
    case SCSIOP_READ_CAPACITY16:
    {
        PREAD_CAPACITY16_DATA readCapacity16;
        UINT64 maxLba64;
        UINT32 sectorSize32;
        ULONG returnedLength;
        ULONG minimumLength;

        if (Cdb->READ_CAPACITY16.ServiceAction != SERVICE_ACTION_READ_CAPACITY16) {
            *SrbStatus = SRB_STATUS_INVALID_REQUEST;
            break;
        }

        minimumLength = sizeof(READ_CAPACITY_DATA_EX);
        if (transferLength < minimumLength) {
            *SrbStatus = SRB_STATUS_DATA_OVERRUN;
            break;
        }

        readCapacity16 = (PREAD_CAPACITY16_DATA)DataBuffer;
        returnedLength = minimumLength;
        RtlZeroMemory(readCapacity16, returnedLength);

        maxLba64 = disk->SectorCount - 1;
        sectorSize32 = disk->SectorSize;
        REVERSE_BYTES_8(&readCapacity16->LogicalBlockAddress.QuadPart, &maxLba64);
        REVERSE_BYTES_4(&readCapacity16->BytesPerBlock, &sectorSize32);

        if (transferLength >= (ULONG)FIELD_OFFSET(READ_CAPACITY16_DATA, Reserved3)) {
            returnedLength = (ULONG)FIELD_OFFSET(READ_CAPACITY16_DATA, Reserved3);
            RtlZeroMemory(readCapacity16, returnedLength);
            REVERSE_BYTES_8(&readCapacity16->LogicalBlockAddress.QuadPart, &maxLba64);
            REVERSE_BYTES_4(&readCapacity16->BytesPerBlock, &sectorSize32);
            readCapacity16->ProtectionEnable = 0;
            readCapacity16->ProtectionType = 0;
            readCapacity16->LogicalPerPhysicalExponent = 0;
            readCapacity16->LowestAlignedBlock_MSB = 0;
            readCapacity16->LowestAlignedBlock_LSB = 0;
            readCapacity16->LBPME = 0;
            readCapacity16->LBPRZ = 0;

            if (transferLength >= sizeof(READ_CAPACITY16_DATA)) {
                returnedLength = sizeof(READ_CAPACITY16_DATA);
                RtlZeroMemory(readCapacity16, returnedLength);
                REVERSE_BYTES_8(&readCapacity16->LogicalBlockAddress.QuadPart, &maxLba64);
                REVERSE_BYTES_4(&readCapacity16->BytesPerBlock, &sectorSize32);
            }
        }

        *DataTransferLength = returnedLength;
        break;
    }
    case SCSIOP_TEST_UNIT_READY:
        break;
    case SCSIOP_REQUEST_SENSE:
    {
        ULONG senseLength;

        senseLength = min(transferLength, 18u);
        if (senseLength != 0) {
            RtlZeroMemory(DataBuffer, senseLength);
            DataBuffer[0] = 0x70;
            if (senseLength > 7) {
                DataBuffer[7] = (UCHAR)(senseLength - 8);
            }
        }
        *DataTransferLength = senseLength;
        break;
    }
    case SCSIOP_MODE_SENSE:
        if (transferLength < 4) {
            *SrbStatus = SRB_STATUS_DATA_OVERRUN;
            break;
        }

        RtlZeroMemory(DataBuffer, 4);
        DataBuffer[0] = 3;
        *DataTransferLength = 4;
        break;
    case SCSIOP_MODE_SENSE10:
        if (transferLength < 8) {
            *SrbStatus = SRB_STATUS_DATA_OVERRUN;
            break;
        }

        RtlZeroMemory(DataBuffer, 8);
        DataBuffer[1] = 6;
        *DataTransferLength = 8;
        break;
    case SCSIOP_MEDIUM_REMOVAL:
    case SCSIOP_START_STOP_UNIT:
    case SCSIOP_SYNCHRONIZE_CACHE:
    case SCSIOP_VERIFY:
    case SCSIOP_VERIFY16:
        break;
    case SCSIOP_READ6:
        startBlockIndex = (((UINT64)Cdb->CDB6READWRITE.LogicalBlockMsb1) << 13) |
            (((UINT64)Cdb->CDB6READWRITE.LogicalBlockMsb0) << 8) |
            Cdb->CDB6READWRITE.LogicalBlockLsb;
        blockCount = Cdb->CDB6READWRITE.TransferBlocks == 0 ? 256 : Cdb->CDB6READWRITE.TransferBlocks;
        goto start_read;
    case SCSIOP_READ:
        REVERSE_BYTES_4(&startBlockIndex, &Cdb->CDB10.LogicalBlockByte0);
        REVERSE_BYTES_2(&blockCount, &Cdb->CDB10.TransferBlocksMsb);
        goto start_read;
    case SCSIOP_READ12:
        REVERSE_BYTES_4(&startBlockIndex, Cdb->READ12.LogicalBlock);
        REVERSE_BYTES_4(&blockCount, Cdb->READ12.TransferLength);
        goto start_read;
    case SCSIOP_READ16:
        REVERSE_BYTES_8(&startBlockIndex, Cdb->READ16.LogicalBlock);
        REVERSE_BYTES_4(&blockCount, Cdb->READ16.TransferLength);
start_read:
        if (!YumeDiskValidateDiskRange(disk, startBlockIndex, blockCount, transferLength, &startByte, &byteCount)) {
            *SrbStatus = SRB_STATUS_INVALID_REQUEST;
            break;
        }

        if (!disk->Present) {
            *SrbStatus = SRB_STATUS_INVALID_TARGET_ID;
            break;
        }

        if (byteCount == 0) {
            *DataTransferLength = 0;
            break;
        }

        ioStatus = YumeDiskQueuePendingScsiIo(
            DeviceExtension,
            extension,
            Srb,
            YumeDiskPendingIoRead,
            TargetId,
            startBlockIndex,
            blockCount,
            (ULONG)byteCount
        );
        if (ioStatus == STATUS_PENDING) {
            *SrbStatus = SRB_STATUS_PENDING;
        } else {
            *SrbStatus = YumeDiskNtStatusToSrbStatus(ioStatus);
        }
        break;
    case SCSIOP_WRITE6:
        startBlockIndex = (((UINT64)Cdb->CDB6READWRITE.LogicalBlockMsb1) << 13) |
            (((UINT64)Cdb->CDB6READWRITE.LogicalBlockMsb0) << 8) |
            Cdb->CDB6READWRITE.LogicalBlockLsb;
        blockCount = Cdb->CDB6READWRITE.TransferBlocks == 0 ? 256 : Cdb->CDB6READWRITE.TransferBlocks;
        goto start_write;
    case SCSIOP_WRITE:
        REVERSE_BYTES_4(&startBlockIndex, &Cdb->CDB10.LogicalBlockByte0);
        REVERSE_BYTES_2(&blockCount, &Cdb->CDB10.TransferBlocksMsb);
        goto start_write;
    case SCSIOP_WRITE12:
        REVERSE_BYTES_4(&startBlockIndex, Cdb->WRITE12.LogicalBlock);
        REVERSE_BYTES_4(&blockCount, Cdb->WRITE12.TransferLength);
        goto start_write;
    case SCSIOP_WRITE16:
        REVERSE_BYTES_8(&startBlockIndex, Cdb->WRITE16.LogicalBlock);
        REVERSE_BYTES_4(&blockCount, Cdb->WRITE16.TransferLength);
start_write:
        if (!YumeDiskValidateDiskRange(disk, startBlockIndex, blockCount, transferLength, &startByte, &byteCount)) {
            *SrbStatus = SRB_STATUS_INVALID_REQUEST;
            break;
        }

        if (!disk->Present) {
            *SrbStatus = SRB_STATUS_INVALID_TARGET_ID;
            break;
        }

        if (byteCount == 0) {
            *DataTransferLength = 0;
            break;
        }

        ioStatus = YumeDiskQueuePendingScsiIo(
            DeviceExtension,
            extension,
            Srb,
            YumeDiskPendingIoWrite,
            TargetId,
            startBlockIndex,
            blockCount,
            (ULONG)byteCount
        );
        if (ioStatus == STATUS_PENDING) {
            *SrbStatus = SRB_STATUS_PENDING;
        } else {
            *SrbStatus = YumeDiskNtStatusToSrbStatus(ioStatus);
        }
        break;
    default:
        *SrbStatus = SRB_STATUS_INVALID_REQUEST;
        break;
    }
}
