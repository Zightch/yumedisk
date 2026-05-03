#include "scsi.h"

#include <ntddscsi.h>

static
VOID
DiskHandleReportLuns(
    _Inout_updates_bytes_(TransferLength) PUCHAR DataBuffer,
    _In_ ULONG TransferLength,
    _Out_ ULONG* DataTransferLength,
    _Out_ UCHAR* SrbStatus
)
{
    REPORT_LUNS_DATA reportLuns;
    UINT32 lunListSize;
    UINT64 lun0;
    ULONG returnLength;

    RtlZeroMemory(&reportLuns, sizeof(reportLuns));
    lunListSize = sizeof(UINT64);
    lun0 = 0;
    returnLength = min(TransferLength, (ULONG)sizeof(reportLuns));

    REVERSE_BYTES_4(&reportLuns.LunListLength, &lunListSize);
    REVERSE_BYTES_8(&reportLuns.Entry[0], &lun0);
    RtlCopyMemory(DataBuffer, &reportLuns, returnLength);
    *DataTransferLength = returnLength;

    if (TransferLength < sizeof(reportLuns)) {
        *SrbStatus = SRB_STATUS_DATA_OVERRUN;
    }
}

static
VOID
DiskHandleInquiry(
    _Inout_updates_bytes_(TransferLength) PUCHAR DataBuffer,
    _In_ ULONG TransferLength,
    _Out_ ULONG* DataTransferLength,
    _Out_ UCHAR* SrbStatus
)
{
    PINQUIRYDATA inquiryData;

    if (TransferLength < INQUIRYDATABUFFERSIZE) {
        *SrbStatus = SRB_STATUS_DATA_OVERRUN;
        return;
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

    RtlCopyMemory(inquiryData->VendorId, "Zightch", 8);
    RtlCopyMemory(inquiryData->ProductId, "YumeDisk        ", 16);
    RtlCopyMemory(inquiryData->ProductRevisionLevel, "1.0 ", 4);
    *DataTransferLength = INQUIRYDATABUFFERSIZE;
}

static
VOID
DiskHandleReadCapacity10(
    _In_ PYUME_DISK Disk,
    _Inout_updates_bytes_(TransferLength) PUCHAR DataBuffer,
    _In_ ULONG TransferLength,
    _Out_ ULONG* DataTransferLength,
    _Out_ UCHAR* SrbStatus
)
{
    PREAD_CAPACITY_DATA readCapacity;
    UINT64 maxLba64;
    UINT32 maxLba32;
    UINT32 sectorSize32;

    if (TransferLength < sizeof(READ_CAPACITY_DATA)) {
        *SrbStatus = SRB_STATUS_DATA_OVERRUN;
        return;
    }

    readCapacity = (PREAD_CAPACITY_DATA)DataBuffer;
    maxLba64 = Disk->SectorCount - 1;
    maxLba32 = (maxLba64 >= MAXULONG) ? MAXULONG : (UINT32)maxLba64;
    sectorSize32 = Disk->SectorSize;

    REVERSE_BYTES_4(&readCapacity->LogicalBlockAddress, &maxLba32);
    REVERSE_BYTES_4(&readCapacity->BytesPerBlock, &sectorSize32);
    *DataTransferLength = sizeof(READ_CAPACITY_DATA);
}

static
VOID
DiskHandleReadCapacity16(
    _In_ PYUME_DISK Disk,
    _In_ PCDB Cdb,
    _Inout_updates_bytes_(TransferLength) PUCHAR DataBuffer,
    _In_ ULONG TransferLength,
    _Out_ ULONG* DataTransferLength,
    _Out_ UCHAR* SrbStatus
)
{
    PREAD_CAPACITY16_DATA readCapacity16;
    UINT64 maxLba64;
    UINT32 sectorSize32;
    ULONG returnedLength;
    ULONG minimumLength;

    if (Cdb->READ_CAPACITY16.ServiceAction != SERVICE_ACTION_READ_CAPACITY16) {
        *SrbStatus = SRB_STATUS_INVALID_REQUEST;
        return;
    }

    minimumLength = sizeof(READ_CAPACITY_DATA_EX);
    if (TransferLength < minimumLength) {
        *SrbStatus = SRB_STATUS_DATA_OVERRUN;
        return;
    }

    readCapacity16 = (PREAD_CAPACITY16_DATA)DataBuffer;
    returnedLength = minimumLength;
    RtlZeroMemory(readCapacity16, returnedLength);

    maxLba64 = Disk->SectorCount - 1;
    sectorSize32 = Disk->SectorSize;
    REVERSE_BYTES_8(&readCapacity16->LogicalBlockAddress.QuadPart, &maxLba64);
    REVERSE_BYTES_4(&readCapacity16->BytesPerBlock, &sectorSize32);

    if (TransferLength >= (ULONG)FIELD_OFFSET(READ_CAPACITY16_DATA, Reserved3)) {
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

        if (TransferLength >= sizeof(READ_CAPACITY16_DATA)) {
            returnedLength = sizeof(READ_CAPACITY16_DATA);
            RtlZeroMemory(readCapacity16, returnedLength);
            REVERSE_BYTES_8(&readCapacity16->LogicalBlockAddress.QuadPart, &maxLba64);
            REVERSE_BYTES_4(&readCapacity16->BytesPerBlock, &sectorSize32);
        }
    }

    *DataTransferLength = returnedLength;
}

static
VOID
DiskHandleRequestSense(
    _Inout_updates_bytes_(TransferLength) PUCHAR DataBuffer,
    _In_ ULONG TransferLength,
    _Out_ ULONG* DataTransferLength
)
{
    ULONG senseLength;

    senseLength = min(TransferLength, 18u);
    if (senseLength != 0) {
        RtlZeroMemory(DataBuffer, senseLength);
        DataBuffer[0] = 0x70;
        if (senseLength > 7) {
            DataBuffer[7] = (UCHAR)(senseLength - 8);
        }
    }

    *DataTransferLength = senseLength;
}

static
VOID
DiskHandleModeSense(
    _Inout_updates_bytes_(RequiredLength) PUCHAR DataBuffer,
    _In_ ULONG TransferLength,
    _In_ ULONG RequiredLength,
    _In_ UCHAR LengthByteIndex,
    _In_ UCHAR LengthValue,
    _Out_ ULONG* DataTransferLength,
    _Out_ UCHAR* SrbStatus
)
{
    if (TransferLength < RequiredLength) {
        *SrbStatus = SRB_STATUS_DATA_OVERRUN;
        return;
    }

    RtlZeroMemory(DataBuffer, RequiredLength);
    DataBuffer[LengthByteIndex] = LengthValue;
    *DataTransferLength = RequiredLength;
}

VOID
DiskHandleScsiCdb(
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

    if (!DiskIsTargetVisible(extension, TargetId)) {
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
        DiskHandleReportLuns(DataBuffer, transferLength, DataTransferLength, SrbStatus);
        break;
    case SCSIOP_INQUIRY:
        DiskHandleInquiry(DataBuffer, transferLength, DataTransferLength, SrbStatus);
        break;
    case SCSIOP_READ_CAPACITY:
        DiskHandleReadCapacity10(disk, DataBuffer, transferLength, DataTransferLength, SrbStatus);
        break;
    case SCSIOP_READ_CAPACITY16:
        DiskHandleReadCapacity16(disk, Cdb, DataBuffer, transferLength, DataTransferLength, SrbStatus);
        break;
    case SCSIOP_TEST_UNIT_READY:
        break;
    case SCSIOP_REQUEST_SENSE:
        DiskHandleRequestSense(DataBuffer, transferLength, DataTransferLength);
        break;
    case SCSIOP_MODE_SENSE:
        DiskHandleModeSense(DataBuffer, transferLength, 4, 0, 3, DataTransferLength, SrbStatus);
        break;
    case SCSIOP_MODE_SENSE10:
        DiskHandleModeSense(DataBuffer, transferLength, 8, 1, 6, DataTransferLength, SrbStatus);
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
        if (!DiskValidateDiskRange(disk, startBlockIndex, blockCount, transferLength, &startByte, &byteCount)) {
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

        ioStatus = DiskQueuePendingScsiIo(
            DeviceExtension,
            extension,
            Srb,
            DiskPendingIoRead,
            TargetId,
            startBlockIndex,
            blockCount,
            (ULONG)byteCount);
        *SrbStatus = (ioStatus == STATUS_PENDING) ? SRB_STATUS_PENDING : DiskNtStatusToSrbStatus(ioStatus);
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
        if (!DiskValidateDiskRange(disk, startBlockIndex, blockCount, transferLength, &startByte, &byteCount)) {
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

        ioStatus = DiskQueuePendingScsiIo(
            DeviceExtension,
            extension,
            Srb,
            DiskPendingIoWrite,
            TargetId,
            startBlockIndex,
            blockCount,
            (ULONG)byteCount);
        *SrbStatus = (ioStatus == STATUS_PENDING) ? SRB_STATUS_PENDING : DiskNtStatusToSrbStatus(ioStatus);
        break;
    default:
        *SrbStatus = SRB_STATUS_INVALID_REQUEST;
        break;
    }
}

