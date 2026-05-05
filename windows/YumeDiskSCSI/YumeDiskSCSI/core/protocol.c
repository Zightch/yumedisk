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
