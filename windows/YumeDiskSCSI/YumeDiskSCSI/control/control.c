#include "control.h"

static
NTSTATUS
DiskValidateWriteAckBatchPayload(
    _In_reads_bytes_(PayloadLength) const UCHAR* Payload,
    _In_ ULONG PayloadLength
)
{
    PYUMEDISK_WRITE_ACK_BATCH batch;
    ULONG expectedLength;

    if (PayloadLength < (ULONG)YUMEDISK_WRITE_ACK_BATCH_BASE_SIZE) {
        return STATUS_INVALID_PARAMETER;
    }

    batch = (PYUMEDISK_WRITE_ACK_BATCH)Payload;
    if (batch->RangeCount > ((MAXULONG - (ULONG)YUMEDISK_WRITE_ACK_BATCH_BASE_SIZE) / (ULONG)sizeof(YUMEDISK_WRITE_ACK_RANGE))) {
        return STATUS_INVALID_PARAMETER;
    }

    expectedLength = (ULONG)YUMEDISK_WRITE_ACK_BATCH_SIZE(batch->RangeCount);
    if (PayloadLength != expectedLength) {
        return STATUS_INVALID_PARAMETER;
    }

    return STATUS_SUCCESS;
}

static
NTSTATUS
DiskHandleQueryInfo(
    _In_ PDEVICE_CONTEXT Extension,
    _Inout_ PYUMEDISK_MESSAGE Message
)
{
    PYUMEDISK_QUERY_INFO info;
    KIRQL oldIrql;

    if (Message->Header.Size < YUMEDISK_MESSAGE_BASE_SIZE + sizeof(YUMEDISK_QUERY_INFO)) {
        return STATUS_BUFFER_TOO_SMALL;
    }

    info = (PYUMEDISK_QUERY_INFO)Message->Payload;
    RtlZeroMemory(info, sizeof(*info));
    info->ProtocolVersion = YUMEDISK_PROTOCOL_VERSION;
    info->MaxTargets = YUMEDISK_USABLE_TARGET_COUNT;
    info->Features = YumeDiskFeatureDynamicDisk | YumeDiskFeatureAppOwnedQueue;
    RtlCopyMemory(info->AdapterSignature, YUMEDISK_MINIPORT_SIGNATURE, sizeof(info->AdapterSignature));
    RtlCopyMemory(info->ServiceName, L"YumeDiskSCSI", sizeof(L"YumeDiskSCSI"));

    DiskInitMessageStatus(Message, YumeDiskCommandQueryInfo, STATUS_SUCCESS, sizeof(YUMEDISK_QUERY_INFO));
    KeAcquireSpinLock(&Extension->SessionLock, &oldIrql);
    if (Extension->CurrentSessionId != 0) {
        Message->Header.SessionId = Extension->CurrentSessionId;
    }
    KeReleaseSpinLock(&Extension->SessionLock, oldIrql);
    return STATUS_SUCCESS;
}

static
NTSTATUS
DiskHandleQueryDebugState(
    _In_ PVOID DeviceExtension,
    _Inout_ PYUMEDISK_MESSAGE Message
)
{
    NTSTATUS status;
    PYUMEDISK_DEBUG_STATE debugState;

    if (Message->Header.PayloadLength != 0) {
        return STATUS_INVALID_PARAMETER;
    }

    if (Message->Header.Size < YUMEDISK_MESSAGE_BASE_SIZE + sizeof(YUMEDISK_DEBUG_STATE)) {
        return STATUS_BUFFER_TOO_SMALL;
    }

    debugState = (PYUMEDISK_DEBUG_STATE)Message->Payload;
    status = DiskQueryDebugState(DeviceExtension, debugState);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    DiskInitMessageStatus(
        Message,
        YumeDiskCommandQueryDebugState,
        STATUS_SUCCESS,
        sizeof(YUMEDISK_DEBUG_STATE));
    return STATUS_SUCCESS;
}

static
NTSTATUS
DiskHandleCreateDisk(
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
        !DiskIsUsableTargetId(request->TargetId) ||
        request->SectorSize == 0 ||
        request->SectorCount == 0 ||
        request->SectorCount > (MAXULONGLONG / request->SectorSize)) {
        return STATUS_INVALID_PARAMETER;
    }

    disk = &Extension->Disk[request->TargetId];
    if (disk->Present) {
        return STATUS_OBJECT_NAME_COLLISION;
    }

    DiskResetDiskStorage(disk);
    diskSize = request->SectorCount * request->SectorSize;
    disk->SectorSize = request->SectorSize;
    disk->SectorCount = request->SectorCount;
    disk->Size = diskSize;
    disk->Configured = TRUE;
    disk->Present = TRUE;
    disk->Removing = FALSE;
    disk->Generation++;

    DiskInitMessageStatus(Message, YumeDiskCommandCreateDisk, STATUS_SUCCESS, 0);
    StorPortNotification(BusChangeDetected, DeviceExtension, 0);
    return STATUS_SUCCESS;
}

static
NTSTATUS
DiskHandleRemoveDisk(
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
    if (request->TargetId >= Extension->MaxTargets ||
        !DiskIsUsableTargetId(request->TargetId)) {
        return STATUS_INVALID_PARAMETER;
    }

    disk = &Extension->Disk[request->TargetId];
    if (!disk->Present) {
        return STATUS_NOT_FOUND;
    }

    disk->Configured = FALSE;
    disk->Present = FALSE;
    disk->Removing = TRUE;
    DiskResetDiskStorage(disk);
    disk->Generation++;
    DiskCompleteTargetPending(DeviceExtension, request->TargetId, STATUS_DEVICE_NOT_CONNECTED);

    DiskInitMessageStatus(Message, YumeDiskCommandRemoveDisk, STATUS_SUCCESS, 0);
    StorPortNotification(BusChangeDetected, DeviceExtension, 0);
    return STATUS_SUCCESS;
}

static
NTSTATUS
DiskHandleRemoveAllDisks(
    _In_ PVOID DeviceExtension,
    _Inout_ PDEVICE_CONTEXT Extension,
    _Inout_ PYUMEDISK_MESSAGE Message
)
{
    ULONG index;
    BOOLEAN closeSession;
    KIRQL oldIrql;

    closeSession = ((Message->Header.Flags & YUMEDISK_SESSION_CLOSE_FLAG) != 0);

    for (index = YUMEDISK_MIN_TARGET_ID; index <= YUMEDISK_MAX_USABLE_TARGET_ID; ++index) {
        if (Extension->Disk[index].Present) {
            Extension->Disk[index].Configured = FALSE;
            Extension->Disk[index].Present = FALSE;
            Extension->Disk[index].Removing = TRUE;
            DiskResetDiskStorage(&Extension->Disk[index]);
            Extension->Disk[index].Generation++;
        }
    }

    DiskCompleteAllPending(DeviceExtension, STATUS_DEVICE_NOT_CONNECTED);
    DiskFreeQueuedState(DeviceExtension);

    if (closeSession) {
        KeAcquireSpinLock(&Extension->SessionLock, &oldIrql);
        Extension->CurrentSessionId = 0;
        Extension->NextEventId = 0;
        KeReleaseSpinLock(&Extension->SessionLock, oldIrql);
    }

    DiskInitMessageStatus(Message, YumeDiskCommandRemoveAllDisks, STATUS_SUCCESS, 0);
    StorPortNotification(BusChangeDetected, DeviceExtension, 0);
    return STATUS_SUCCESS;
}

static
NTSTATUS
DiskHandleSubmitSlot(
    _In_ PVOID DeviceExtension,
    _In_ PSTORAGE_REQUEST_BLOCK Srb,
    _Inout_ PYUMEDISK_MESSAGE Message,
    _Out_ BOOLEAN* RequestCompleted
)
{
    PYUMEDISK_SUBMIT_SLOT submitSlot;
    ULONG expectedLength;

    if (Message->Header.PayloadLength < (ULONG)YUMEDISK_SUBMIT_SLOT_BASE_SIZE) {
        return STATUS_INVALID_PARAMETER;
    }

    submitSlot = (PYUMEDISK_SUBMIT_SLOT)Message->Payload;
    if (submitSlot->Slot.SessionId == 0 ||
        submitSlot->Slot.SessionId != Message->Header.SessionId ||
        submitSlot->Slot.TargetId > YUMEDISK_MAX_USABLE_TARGET_ID ||
        submitSlot->Slot.KernelVa == 0 ||
        submitSlot->Slot.Capacity == 0 ||
        submitSlot->Slot.Flags != YumeDiskSlotFlagNone ||
        (submitSlot->Slot.SlotType != YumeDiskSlotTypeRead &&
            submitSlot->Slot.SlotType != YumeDiskSlotTypeWrite)) {
        return STATUS_INVALID_PARAMETER;
    }

    expectedLength = (ULONG)YUMEDISK_SUBMIT_SLOT_SIZE();
    if (Message->Header.PayloadLength != expectedLength) {
        return STATUS_INVALID_PARAMETER;
    }

    return DiskQueueSubmitSlot(DeviceExtension, Srb, Message, RequestCompleted);
}

static
NTSTATUS
DiskHandleReadAck(
    _In_ PVOID DeviceExtension,
    _Inout_ PYUMEDISK_MESSAGE Message
)
{
    PYUMEDISK_READ_ACK readAck;

    if (Message->Header.PayloadLength != sizeof(YUMEDISK_READ_ACK)) {
        return STATUS_INVALID_PARAMETER;
    }

    readAck = (PYUMEDISK_READ_ACK)Message->Payload;
    if (readAck->EventId == 0 ||
        (readAck->DataLength != 0 && readAck->KernelVa == 0)) {
        return STATUS_INVALID_PARAMETER;
    }

    return DiskQueueReadAck(DeviceExtension, Message);
}

static
NTSTATUS
DiskHandleWriteAckBatch(
    _In_ PVOID DeviceExtension,
    _In_ PSTORAGE_REQUEST_BLOCK Srb,
    _Inout_ PYUMEDISK_MESSAGE Message,
    _Out_ BOOLEAN* RequestCompleted
)
{
    NTSTATUS status;

    status = DiskValidateWriteAckBatchPayload(Message->Payload, Message->Header.PayloadLength);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    return DiskQueueWriteAckBatch(DeviceExtension, Srb, Message, RequestCompleted);
}

static
NTSTATUS
DiskHandleCancelSlot(
    _In_ PVOID DeviceExtension,
    _Inout_ PYUMEDISK_MESSAGE Message
)
{
    PYUMEDISK_CANCEL_SLOT cancelSlot;

    if (Message->Header.PayloadLength != sizeof(YUMEDISK_CANCEL_SLOT)) {
        return STATUS_INVALID_PARAMETER;
    }

    cancelSlot = (PYUMEDISK_CANCEL_SLOT)Message->Payload;
    if (cancelSlot->SlotId == 0 ||
        cancelSlot->TargetId > YUMEDISK_MAX_USABLE_TARGET_ID ||
        (cancelSlot->SlotType != YumeDiskSlotTypeRead &&
            cancelSlot->SlotType != YumeDiskSlotTypeWrite)) {
        return STATUS_INVALID_PARAMETER;
    }

    return DiskQueueCancelSlot(DeviceExtension, Message);
}

BOOLEAN
DiskHandleIoControlSrb(
    _In_ PVOID DeviceExtension,
    _In_ PSTORAGE_REQUEST_BLOCK Srb
)
{
    PDEVICE_CONTEXT extension;
    PSRB_IO_CONTROL srbIoControl;
    PYUMEDISK_MESSAGE message;
    NTSTATUS status;
    KIRQL oldIrql;
    BOOLEAN requestCompleted;

    extension = (PDEVICE_CONTEXT)DeviceExtension;

    if (Srb->DataBuffer == NULL ||
        Srb->DataTransferLength < (ULONG)(sizeof(SRB_IO_CONTROL) + YUMEDISK_MESSAGE_BASE_SIZE)) {
        Srb->SrbStatus = SRB_STATUS_INVALID_REQUEST;
        StorPortNotification(RequestComplete, DeviceExtension, Srb);
        return TRUE;
    }

    srbIoControl = (PSRB_IO_CONTROL)Srb->DataBuffer;
    if (srbIoControl->HeaderLength != sizeof(SRB_IO_CONTROL) ||
        RtlCompareMemory(
            srbIoControl->Signature,
            YUMEDISK_MINIPORT_SIGNATURE,
            sizeof(srbIoControl->Signature)) != sizeof(srbIoControl->Signature)) {
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
        DiskInitMessageStatus(message, message->Header.Command, STATUS_REVISION_MISMATCH, 0);
        DiskCompleteIoctlSrb(Srb, srbIoControl, STATUS_REVISION_MISMATCH, message->Header.Size);
        StorPortNotification(RequestComplete, DeviceExtension, Srb);
        return TRUE;
    }

    KeAcquireSpinLock(&extension->SessionLock, &oldIrql);
    status = DiskClaimSessionLocked(extension, &message->Header);
    KeReleaseSpinLock(&extension->SessionLock, oldIrql);

    if (!NT_SUCCESS(status)) {
        DiskInitMessageStatus(message, message->Header.Command, status, 0);
        DiskCompleteIoctlSrb(Srb, srbIoControl, status, message->Header.Size);
        StorPortNotification(RequestComplete, DeviceExtension, Srb);
        return TRUE;
    }

    requestCompleted = FALSE;
    switch (message->Header.Command) {
    case YumeDiskCommandQueryInfo:
        status = DiskHandleQueryInfo(extension, message);
        break;
    case YumeDiskCommandQueryDebugState:
        status = DiskHandleQueryDebugState(DeviceExtension, message);
        break;
    case YumeDiskCommandCreateDisk:
        status = DiskHandleCreateDisk(DeviceExtension, extension, message);
        break;
    case YumeDiskCommandRemoveDisk:
        status = DiskHandleRemoveDisk(DeviceExtension, extension, message);
        break;
    case YumeDiskCommandRemoveAllDisks:
        status = DiskHandleRemoveAllDisks(DeviceExtension, extension, message);
        break;
    case YumeDiskCommandSubmitSlot:
        status = DiskHandleSubmitSlot(DeviceExtension, Srb, message, &requestCompleted);
        if (requestCompleted) {
            return TRUE;
        }
        break;
    case YumeDiskCommandReadAck:
        status = DiskHandleReadAck(DeviceExtension, message);
        break;
    case YumeDiskCommandWriteAckBatch:
        status = DiskHandleWriteAckBatch(DeviceExtension, Srb, message, &requestCompleted);
        if (requestCompleted) {
            return TRUE;
        }
        break;
    case YumeDiskCommandCancelSlot:
        status = DiskHandleCancelSlot(DeviceExtension, message);
        break;
    default:
        DiskInitMessageStatus(message, message->Header.Command, STATUS_INVALID_DEVICE_REQUEST, 0);
        status = STATUS_INVALID_DEVICE_REQUEST;
        break;
    }

    if (status == STATUS_PENDING) {
        Srb->SrbStatus = SRB_STATUS_PENDING;
        return TRUE;
    }

    if (!NT_SUCCESS(status) && message->Header.Status == STATUS_SUCCESS) {
        DiskInitMessageStatus(message, message->Header.Command, status, 0);
    }

    DiskCompleteIoctlSrb(Srb, srbIoControl, status, message->Header.Size);
    StorPortNotification(RequestComplete, DeviceExtension, Srb);
    return TRUE;
}
