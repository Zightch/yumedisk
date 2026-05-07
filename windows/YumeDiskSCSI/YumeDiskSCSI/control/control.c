#include "control.h"

static
NTSTATUS
DiskHandleQueryScsiInfo(
    _In_ PDEVICE_CONTEXT Extension,
    _Inout_ PYUMEDISK_MESSAGE Message
)
{
    PYUMEDISK_SCSI_INFO info;
    KIRQL oldIrql;

    if (Message->Header.Size < YUMEDISK_MESSAGE_BASE_SIZE + sizeof(YUMEDISK_SCSI_INFO)) {
        return STATUS_BUFFER_TOO_SMALL;
    }

    info = (PYUMEDISK_SCSI_INFO)Message->Payload;
    RtlZeroMemory(info, sizeof(*info));
    info->VersionBe = YUMEDISK_COMPONENT_VERSION_BE;
    RtlCopyMemory(info->AdapterSignature, YUMEDISK_MINIPORT_SIGNATURE, sizeof(info->AdapterSignature));

    DiskInitMessageStatus(Message, YumeDiskCommandQueryScsiInfo, STATUS_SUCCESS, sizeof(YUMEDISK_SCSI_INFO));
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
    disk->ReadOnly = request->ReadOnly != 0u ? TRUE : FALSE;
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
    disk->ReadOnly = FALSE;
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
            Extension->Disk[index].ReadOnly = FALSE;
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
    LIST_ENTRY deferredWriteCompletions;

    extension = (PDEVICE_CONTEXT)DeviceExtension;
    InitializeListHead(&deferredWriteCompletions);

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
    if (message->Header.Size < (ULONG)YUMEDISK_MESSAGE_BASE_SIZE ||
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

    switch (message->Header.Command) {
    case YumeDiskCommandQueryScsiInfo:
        status = DiskHandleQueryScsiInfo(extension, message);
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
        Srb->SrbStatus = SRB_STATUS_PENDING;
        status = DiskHandleSubmitSlotIoctl(DeviceExtension, Srb, message);
        if (status == STATUS_PENDING) {
            return TRUE;
        }
        DiskInitMessageStatus(message, message->Header.Command, status, 0);
        break;
    case YumeDiskCommandReadAck:
        status = DiskHandleReadAckIoctl(DeviceExtension, message);
        if (!NT_SUCCESS(status)) {
            DiskInitMessageStatus(message, message->Header.Command, status, 0);
        }
        break;
    case YumeDiskCommandWriteAckBatch:
        status = DiskHandleWriteAckBatchIoctl(DeviceExtension, message, &deferredWriteCompletions);
        if (!NT_SUCCESS(status)) {
            DiskInitMessageStatus(message, message->Header.Command, status, 0);
        }
        break;
    case YumeDiskCommandCancelSlot:
        status = DiskHandleCancelSlotIoctl(DeviceExtension, message);
        if (!NT_SUCCESS(status)) {
            DiskInitMessageStatus(message, message->Header.Command, status, 0);
        }
        break;
    default:
        status = STATUS_INVALID_DEVICE_REQUEST;
        DiskInitMessageStatus(message, message->Header.Command, status, 0);
        break;
    }

    DiskCompleteIoctlSrb(Srb, srbIoControl, status, message->Header.Size);
    StorPortNotification(RequestComplete, DeviceExtension, Srb);
    DiskCompleteDeferredWriteCompletions(DeviceExtension, &deferredWriteCompletions);
    return TRUE;
}
