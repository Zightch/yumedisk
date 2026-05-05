#include "protocol.h"

#include <ntddscsi.h>

#include "memory.h"

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
    Message->Header.Version = YUMEDISK_PROTOCOL_VERSION;
    Message->Header.Command = Command;
    Message->Header.Status = Status;
    Message->Header.PayloadLength = PayloadLength;
    Message->Header.Size = YUMEDISK_MESSAGE_BASE_SIZE + PayloadLength;
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

ULONGLONG
DiskAllocateTxIdLocked(
    _Inout_ PDEVICE_CONTEXT Extension
)
{
    return ++Extension->NextTxId;
}

VOID
DiskAssignEventTxId(
    _In_ PDEVICE_CONTEXT Extension,
    _Inout_ YUMEDISK_EVENT* Event
)
{
    KIRQL oldIrql;

    KeAcquireSpinLock(&Extension->ControlLock, &oldIrql);
    Event->TxId = DiskAllocateTxIdLocked(Extension);
    KeReleaseSpinLock(&Extension->ControlLock, oldIrql);
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

NTSTATUS
DiskClaimSessionLocked(
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

PSRBEX_DATA_SCSI_CDB16
DiskGetScsiCdb16Data(
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

UCHAR
DiskNtStatusToSrbStatus(
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

VOID
DiskCompleteScsiSrb(
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
        DiskFree(PendingIo);
        return;
    }

    cdb16 = DiskGetScsiCdb16Data(srb);
    if (NT_SUCCESS(Status) && PendingIo->Type == DiskPendingIoRead && DataLength != 0) {
        if (srb->DataBuffer == NULL || Data == NULL) {
            Status = STATUS_INVALID_PARAMETER;
        } else {
            RtlCopyMemory(srb->DataBuffer, Data, DataLength);
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
        srb->SrbStatus = DiskNtStatusToSrbStatus(Status);
        if (cdb16 != NULL) {
            cdb16->ScsiStatus = SCSISTAT_GOOD;
        }
    }

    StorPortNotification(RequestComplete, DeviceExtension, srb);
    DiskFree(PendingIo);
}

BOOLEAN
DiskValidateDiskRange(
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

