#include "control.h"

static
NTSTATUS
DiskHandleQueryInfo(
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
    info->MaxTargets = YUMEDISK_USABLE_TARGET_COUNT;
    info->Features = YumeDiskFeatureDynamicDisk;
    RtlCopyMemory(info->AdapterSignature, YUMEDISK_MINIPORT_SIGNATURE, sizeof(info->AdapterSignature));
    RtlCopyMemory(info->ServiceName, L"YumeDiskSCSI", sizeof(L"YumeDiskSCSI"));

    DiskInitMessageStatus(Message, YumeDiskCommandQueryInfo, STATUS_SUCCESS, sizeof(YUMEDISK_QUERY_INFO));
    if (Extension->CurrentSessionId != 0) {
        Message->Header.SessionId = Extension->CurrentSessionId;
    }
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

    if (closeSession) {
        DiskFreeQueuedState(DeviceExtension);
        Extension->CurrentSessionId = 0;
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

    KeAcquireSpinLock(&extension->ControlLock, &oldIrql);
    status = DiskClaimSessionLocked(extension, &message->Header);
    KeReleaseSpinLock(&extension->ControlLock, oldIrql);

    if (!NT_SUCCESS(status)) {
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

