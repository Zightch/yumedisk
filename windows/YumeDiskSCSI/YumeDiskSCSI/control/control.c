#include "control.h"

#include <srbhelper.h>

static
NTSTATUS
DiskHandleQueryInfo(
    _In_ PDEVICE_CONTEXT Extension,
    _Inout_ PYUMEDISK_MESSAGE Message
)
{
    PYUMEDISK_QUERY_INFO info;

    YD_SCSI_LOG(
        "DiskHandleQueryInfo enter, sessionId=%I64u, messageSize=%lu",
        Message->Header.SessionId,
        Message->Header.Size);
    if (Message->Header.Size < YUMEDISK_MESSAGE_BASE_SIZE + sizeof(YUMEDISK_QUERY_INFO)) {
        YD_SCSI_ERR("DiskHandleQueryInfo buffer too small, size=%lu", Message->Header.Size);
        return STATUS_BUFFER_TOO_SMALL;
    }

    info = (PYUMEDISK_QUERY_INFO)Message->Payload;
    RtlZeroMemory(info, sizeof(*info));
    info->ProtocolVersion = YUMEDISK_PROTOCOL_VERSION;
    info->MaxTargets = YUMEDISK_USABLE_TARGET_COUNT;
    info->Features = YumeDiskFeatureWaitEvent | YumeDiskFeatureDynamicDisk | YumeDiskFeatureIoSkeleton;
    RtlCopyMemory(info->AdapterSignature, YUMEDISK_MINIPORT_SIGNATURE, sizeof(info->AdapterSignature));
    RtlCopyMemory(info->ServiceName, L"YumeDiskSCSI", sizeof(L"YumeDiskSCSI"));

    DiskInitMessageStatus(Message, YumeDiskCommandQueryInfo, STATUS_SUCCESS, sizeof(YUMEDISK_QUERY_INFO));
    if (Extension->CurrentSessionId != 0) {
        Message->Header.SessionId = Extension->CurrentSessionId;
    }
    YD_SCSI_LOG(
        "DiskHandleQueryInfo ok, currentSessionId=%I64u, service=%ws",
        Message->Header.SessionId,
        info->ServiceName);
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
    DiskQueueSyntheticEvent(DeviceExtension, YumeDiskEventDiskAdded, request->TargetId);
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
    DiskCancelPendingIoByTarget(DeviceExtension, Extension, request->TargetId, STATUS_DEVICE_NOT_CONNECTED);
    DiskResetDiskStorage(disk);
    disk->Generation++;

    DiskInitMessageStatus(Message, YumeDiskCommandRemoveDisk, STATUS_SUCCESS, 0);
    DiskQueueSyntheticEvent(DeviceExtension, YumeDiskEventDiskRemoved, request->TargetId);
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

    for (index = YUMEDISK_MIN_TARGET_ID; index <= YUMEDISK_MAX_USABLE_TARGET_ID; ++index) {
        if (Extension->Disk[index].Present) {
            Extension->Disk[index].Configured = FALSE;
            Extension->Disk[index].Present = FALSE;
            Extension->Disk[index].Removing = TRUE;
            DiskCancelPendingIoByTarget(DeviceExtension, Extension, index, STATUS_DEVICE_NOT_CONNECTED);
            DiskResetDiskStorage(&Extension->Disk[index]);
            Extension->Disk[index].Generation++;
            DiskQueueSyntheticEvent(DeviceExtension, YumeDiskEventDiskRemoved, index);
        }
    }

    if ((Message->Header.Flags & YUMEDISK_SESSION_CLOSE_FLAG) != 0) {
        DiskQueueSyntheticEvent(DeviceExtension, YumeDiskEventShutdown, 0);
        Extension->CurrentSessionId = 0;
        DiskFreeQueuedState(DeviceExtension);
        DiskCompleteAllPending(DeviceExtension, STATUS_DEVICE_NOT_CONNECTED);
    }

    DiskInitMessageStatus(Message, YumeDiskCommandRemoveAllDisks, STATUS_SUCCESS, 0);
    StorPortNotification(BusChangeDetected, DeviceExtension, 0);
    return STATUS_SUCCESS;
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

    extension = (PDEVICE_CONTEXT)DeviceExtension;
    SrbSetSrbStatus(Srb, SRB_STATUS_SUCCESS);
    SrbSetScsiStatus(Srb, SCSISTAT_GOOD);
    YD_SCSI_LOG(
        "DiskHandleIoControlSrb enter, srb=%p dataBuffer=%p dataTransfer=%lu",
        Srb,
        Srb->DataBuffer,
        Srb->DataTransferLength);

    if (Srb->DataBuffer == NULL ||
        Srb->DataTransferLength < (ULONG)(sizeof(SRB_IO_CONTROL) + YUMEDISK_MESSAGE_BASE_SIZE)) {
        YD_SCSI_ERR(
            "DiskHandleIoControlSrb invalid request, dataBuffer=%p dataTransfer=%lu",
            Srb->DataBuffer,
            Srb->DataTransferLength);
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
        YD_SCSI_ERR(
            "DiskHandleIoControlSrb signature/header mismatch, headerLength=%lu controlCode=0x%08X length=%lu",
            srbIoControl->HeaderLength,
            srbIoControl->ControlCode,
            srbIoControl->Length);
        Srb->SrbStatus = SRB_STATUS_INVALID_REQUEST;
        StorPortNotification(RequestComplete, DeviceExtension, Srb);
        return TRUE;
    }

    if (Srb->DataTransferLength < (ULONG)(sizeof(SRB_IO_CONTROL) + srbIoControl->Length) ||
        srbIoControl->Length < (ULONG)YUMEDISK_MESSAGE_BASE_SIZE) {
        YD_SCSI_ERR(
            "DiskHandleIoControlSrb invalid length, transfer=%lu ioctlLength=%lu",
            Srb->DataTransferLength,
            srbIoControl->Length);
        Srb->SrbStatus = SRB_STATUS_INVALID_REQUEST;
        StorPortNotification(RequestComplete, DeviceExtension, Srb);
        return TRUE;
    }

    message = (PYUMEDISK_MESSAGE)(srbIoControl + 1);
    if (message->Header.Version != YUMEDISK_PROTOCOL_VERSION ||
        message->Header.Size < (ULONG)YUMEDISK_MESSAGE_BASE_SIZE ||
        message->Header.Size > srbIoControl->Length ||
        (ULONG)(YUMEDISK_MESSAGE_BASE_SIZE + message->Header.PayloadLength) > message->Header.Size) {
        YD_SCSI_ERR(
            "DiskHandleIoControlSrb invalid message header, cmd=%lu version=%lu size=%lu payload=%lu ioctlLength=%lu",
            message->Header.Command,
            message->Header.Version,
            message->Header.Size,
            message->Header.PayloadLength,
            srbIoControl->Length);
        DiskInitMessageStatus(message, message->Header.Command, STATUS_REVISION_MISMATCH, 0);
        DiskCompleteIoctlSrb(Srb, srbIoControl, STATUS_REVISION_MISMATCH, message->Header.Size);
        StorPortNotification(RequestComplete, DeviceExtension, Srb);
        return TRUE;
    }

    YD_SCSI_LOG(
        "DiskHandleIoControlSrb message, cmd=%lu session=%I64u size=%lu payload=%lu flags=0x%08X",
        message->Header.Command,
        message->Header.SessionId,
        message->Header.Size,
        message->Header.PayloadLength,
        message->Header.Flags);

    KeAcquireSpinLock(&extension->ControlLock, &oldIrql);
    status = DiskClaimSessionLocked(extension, &message->Header);
    KeReleaseSpinLock(&extension->ControlLock, oldIrql);

    if (!NT_SUCCESS(status)) {
        YD_SCSI_ERR(
            "DiskClaimSessionLocked failed, cmd=%lu session=%I64u status=0x%08X currentSession=%I64u",
            message->Header.Command,
            message->Header.SessionId,
            status,
            extension->CurrentSessionId);
        DiskInitMessageStatus(message, message->Header.Command, status, 0);
        DiskCompleteIoctlSrb(Srb, srbIoControl, status, message->Header.Size);
        StorPortNotification(RequestComplete, DeviceExtension, Srb);
        return TRUE;
    }

    switch (message->Header.Command) {
    case YumeDiskCommandQueryInfo:
        status = DiskHandleQueryInfo(extension, message);
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
    case YumeDiskCommandWaitEvent:
        status = DiskHandleWaitEvent(DeviceExtension, extension, message, Srb);
        break;
    case YumeDiskCommandHeartbeat:
        DiskInitMessageStatus(message, YumeDiskCommandHeartbeat, STATUS_SUCCESS, 0);
        status = STATUS_SUCCESS;
        break;
    case YumeDiskCommandReadReply:
        status = DiskHandleReadReply(DeviceExtension, extension, message);
        break;
    case YumeDiskCommandWriteAck:
        status = DiskHandleWriteAck(DeviceExtension, extension, message);
        break;
    default:
        DiskInitMessageStatus(message, message->Header.Command, STATUS_INVALID_DEVICE_REQUEST, 0);
        status = STATUS_INVALID_DEVICE_REQUEST;
        break;
    }

    YD_SCSI_LOG(
        "DiskHandleIoControlSrb complete, cmd=%lu status=0x%08X replyStatus=0x%08X replySize=%lu",
        message->Header.Command,
        status,
        message->Header.Status,
        message->Header.Size);

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

