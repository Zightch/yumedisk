#include "protocol.h"

#include <ntddscsi.h>

#include "memory.h"

static
PSRBEX_DATA_SCSI_CDB16
DiskGetScsiCdbExData(
    _In_ PSTORAGE_REQUEST_BLOCK Srb
)
{
    PUCHAR base;
    ULONG offset;
    PSRBEX_DATA exData;

    if (Srb == NULL ||
        Srb->NumSrbExData != 1 ||
        Srb->SrbExDataOffset == NULL) {
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
DiskMapNtStatusToSrbStatus(
    _In_ NTSTATUS Status
)
{
    if (NT_SUCCESS(Status)) {
        return SRB_STATUS_SUCCESS;
    }

    switch (Status) {
    case STATUS_CANCELLED:
        return SRB_STATUS_ABORTED;
    case STATUS_MEDIA_WRITE_PROTECTED:
        return SRB_STATUS_ERROR;
    case STATUS_DEVICE_DOES_NOT_EXIST:
    case STATUS_DEVICE_NOT_CONNECTED:
    case STATUS_NO_SUCH_DEVICE:
    case STATUS_NOT_FOUND:
        return SRB_STATUS_NO_DEVICE;
    case STATUS_BUFFER_TOO_SMALL:
    case STATUS_INVALID_PARAMETER:
    case STATUS_INVALID_DEVICE_REQUEST:
        return SRB_STATUS_INVALID_REQUEST;
    default:
        return SRB_STATUS_ERROR;
    }
}

static
VOID
DiskFillSenseBuffer(
    _Inout_ PSRBEX_DATA_SCSI_CDB16 Cdb16,
    _In_ NTSTATUS Status
)
{
    PUCHAR senseBuffer;
    UCHAR senseLength;
    UCHAR senseKey;

    if (Cdb16 == NULL || Cdb16->SenseInfoBuffer == NULL || Cdb16->SenseInfoBufferLength == 0) {
        return;
    }

    senseBuffer = (PUCHAR)Cdb16->SenseInfoBuffer;
    senseLength = min(Cdb16->SenseInfoBufferLength, 18u);
    RtlZeroMemory(senseBuffer, senseLength);

    if (Status == STATUS_MEDIA_WRITE_PROTECTED) {
        senseKey = SCSI_SENSE_DATA_PROTECT;
    } else if (Status == STATUS_DEVICE_DOES_NOT_EXIST ||
        Status == STATUS_DEVICE_NOT_CONNECTED ||
        Status == STATUS_NO_SUCH_DEVICE) {
        senseKey = SCSI_SENSE_NOT_READY;
    } else if (Status == STATUS_INVALID_PARAMETER ||
        Status == STATUS_INVALID_DEVICE_REQUEST ||
        Status == STATUS_BUFFER_TOO_SMALL) {
        senseKey = SCSI_SENSE_ILLEGAL_REQUEST;
    } else {
        senseKey = SCSI_SENSE_HARDWARE_ERROR;
    }

    senseBuffer[0] = 0x70;
    if (senseLength > 2) {
        senseBuffer[2] = senseKey;
    }
    if (senseLength > 7) {
        senseBuffer[7] = (UCHAR)(senseLength - 8);
    }
    if (Status == STATUS_MEDIA_WRITE_PROTECTED) {
        if (senseLength > 12) {
            senseBuffer[12] = 0x27;
        }
        if (senseLength > 13) {
            senseBuffer[13] = 0x00;
        }
    }
    Cdb16->SenseInfoBufferLength = senseLength;
}

BOOLEAN
DiskIsUsableTargetId(
    _In_ ULONG TargetId
)
{
    return TargetId <= YUMEDISK_MAX_USABLE_TARGET_ID;
}

BOOLEAN
DiskIsTargetVisible(
    _In_ PDEVICE_CONTEXT Extension,
    _In_ UCHAR TargetId
)
{
    if (TargetId >= Extension->MaxTargets || !DiskIsUsableTargetId(TargetId)) {
        return FALSE;
    }

    return Extension->Disk[TargetId].Configured && Extension->Disk[TargetId].Present;
}

VOID
DiskInitMessageStatus(
    _Inout_ PYUMEDISK_MESSAGE Message,
    _In_ ULONG Command,
    _In_ NTSTATUS Status,
    _In_ ULONG PayloadLength
)
{
    Message->Header.Command = Command;
    Message->Header.Status = Status;
    Message->Header.Reserved0 = 0u;
    Message->Header.PayloadLength = PayloadLength;
    Message->Header.Size = YUMEDISK_MESSAGE_BASE_SIZE + PayloadLength;
    Message->Header.Reserved1 = 0u;
}

VOID
DiskCompleteIoctlSrb(
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

VOID
DiskCompleteScsiSrb(
    _In_ PVOID DeviceExtension,
    _In_ PSTORAGE_REQUEST_BLOCK Srb,
    _In_ NTSTATUS Status,
    _In_ ULONG DataTransferLength
)
{
    PSRBEX_DATA_SCSI_CDB16 cdb16;

    cdb16 = DiskGetScsiCdbExData(Srb);
    if (NT_SUCCESS(Status)) {
        Srb->SrbStatus = SRB_STATUS_SUCCESS;
        Srb->DataTransferLength = DataTransferLength;
        if (cdb16 != NULL) {
            cdb16->ScsiStatus = SCSISTAT_GOOD;
            cdb16->SenseInfoBufferLength = 0;
        }
    } else {
        Srb->SrbStatus = DiskMapNtStatusToSrbStatus(Status);
        Srb->DataTransferLength = 0;
        if (cdb16 != NULL) {
            cdb16->ScsiStatus = SCSISTAT_CHECK_CONDITION;
            DiskFillSenseBuffer(cdb16, Status);
        }
    }

    StorPortNotification(RequestComplete, DeviceExtension, Srb);
}

VOID
DiskResetDiskStorage(
    _Inout_ PYUME_DISK Disk
)
{
    KIRQL oldIrql;

    KeAcquireSpinLock(&Disk->BufferLock, &oldIrql);
    if (Disk->Buffer != NULL) {
        DiskFree(Disk->Buffer);
        Disk->Buffer = NULL;
    }
    KeReleaseSpinLock(&Disk->BufferLock, oldIrql);
}

BOOLEAN
DiskTryMarkPendingDataChangedUa(
    _Inout_ PYUME_DISK Disk
)
{
    KIRQL oldIrql;
    BOOLEAN marked;

    marked = FALSE;
    KeAcquireSpinLock(&Disk->BufferLock, &oldIrql);
    if (!Disk->PendingDataChangedUa) {
        Disk->PendingDataChangedUa = TRUE;
        marked = TRUE;
    }
    KeReleaseSpinLock(&Disk->BufferLock, oldIrql);
    return marked;
}

BOOLEAN
DiskTryConsumePendingDataChangedUa(
    _Inout_ PYUME_DISK Disk
)
{
    KIRQL oldIrql;
    BOOLEAN consumed;

    consumed = FALSE;
    KeAcquireSpinLock(&Disk->BufferLock, &oldIrql);
    if (Disk->PendingDataChangedUa) {
        Disk->PendingDataChangedUa = FALSE;
        consumed = TRUE;
    }
    KeReleaseSpinLock(&Disk->BufferLock, oldIrql);
    return consumed;
}

static
VOID
DiskBuildTargetAddress(
    _In_ UCHAR TargetId,
    _Out_ PSTOR_ADDR_BTL8 Address
)
{
    RtlZeroMemory(Address, sizeof(*Address));
    Address->Type = STOR_ADDRESS_TYPE_BTL8;
    Address->AddressLength = STOR_ADDR_BTL8_ADDRESS_LENGTH;
    Address->Path = 0;
    Address->Target = TargetId;
    Address->Lun = 0;
}

ULONG
DiskRegisterTargetAsyncNotifications(
    _In_ PVOID DeviceExtension,
    _In_ UCHAR TargetId
)
{
    STOR_ADDR_BTL8 address;
    STOR_UNIT_ATTRIBUTES attributes;

    DiskBuildTargetAddress(TargetId, &address);
    RtlZeroMemory(&attributes, sizeof(attributes));
    attributes.DeviceAttentionSupported = 1;
    attributes.AsyncNotificationSupported = 1;

    return StorPortSetUnitAttributes(DeviceExtension, (PSTOR_ADDRESS)&address, attributes);
}

ULONG
DiskNotifyTargetMediaStatus(
    _In_ PVOID DeviceExtension,
    _In_ UCHAR TargetId
)
{
    STOR_ADDR_BTL8 address;

    DiskBuildTargetAddress(TargetId, &address);
    return StorPortAsyncNotificationDetected(
        DeviceExtension,
        (PSTOR_ADDRESS)&address,
        RAID_ASYNC_NOTIFY_FLAG_MEDIA_STATUS);
}

NTSTATUS
DiskClaimSessionLocked(
    _Inout_ PDEVICE_CONTEXT Extension,
    _In_ const YUMEDISK_HEADER* Header
)
{
    if (Header->Command == YumeDiskCommandQueryScsiInfo) {
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
