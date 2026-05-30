#include "scsi.h"

#include <ntddscsi.h>

static
UINT32
DiskReadBigEndian32(
    _In_reads_(4) const UCHAR* Bytes
)
{
    return ((UINT32)Bytes[0] << 24) |
        ((UINT32)Bytes[1] << 16) |
        ((UINT32)Bytes[2] << 8) |
        Bytes[3];
}

static
UINT64
DiskReadBigEndian64(
    _In_reads_(8) const UCHAR* Bytes
)
{
    return ((UINT64)Bytes[0] << 56) |
        ((UINT64)Bytes[1] << 48) |
        ((UINT64)Bytes[2] << 40) |
        ((UINT64)Bytes[3] << 32) |
        ((UINT64)Bytes[4] << 24) |
        ((UINT64)Bytes[5] << 16) |
        ((UINT64)Bytes[6] << 8) |
        Bytes[7];
}

static
BOOLEAN
DiskTryParseReadWriteCdb(
    _In_ const YUME_DISK* Disk,
    _In_ PCDB Cdb,
    _In_ ULONG TransferLength,
    _Out_ UINT64* Lba,
    _Out_ ULONG* BlockCount
)
{
    const UCHAR* bytes;
    UCHAR opcode;
    UINT64 localLba;
    ULONG localBlockCount;
    ULONGLONG expectedLength;

    bytes = Cdb->AsByte;
    opcode = bytes[0];
    localLba = 0;
    localBlockCount = 0;

    switch (opcode) {
    case SCSIOP_READ6:
    case SCSIOP_WRITE6:
        localLba = ((UINT32)(bytes[1] & 0x1f) << 16) |
            ((UINT32)bytes[2] << 8) |
            bytes[3];
        localBlockCount = (bytes[4] == 0) ? 256u : bytes[4];
        break;
    case SCSIOP_READ:
    case SCSIOP_WRITE:
        localLba = DiskReadBigEndian32(&bytes[2]);
        localBlockCount = ((UINT32)bytes[7] << 8) | bytes[8];
        break;
    case SCSIOP_READ12:
    case SCSIOP_WRITE12:
        localLba = DiskReadBigEndian32(&bytes[2]);
        localBlockCount = DiskReadBigEndian32(&bytes[6]);
        break;
    case SCSIOP_READ16:
    case SCSIOP_WRITE16:
        localLba = DiskReadBigEndian64(&bytes[2]);
        localBlockCount = DiskReadBigEndian32(&bytes[10]);
        break;
    default:
        return FALSE;
    }

    expectedLength = (ULONGLONG)localBlockCount * Disk->SectorSize;
    if (expectedLength != TransferLength) {
        return FALSE;
    }

    *Lba = localLba;
    *BlockCount = localBlockCount;
    return TRUE;
}

static
UCHAR
DiskMapQueueFailureToSrbStatus(
    _In_ NTSTATUS Status
)
{
    switch (Status) {
    case STATUS_DEVICE_NOT_CONNECTED:
    case STATUS_DEVICE_DOES_NOT_EXIST:
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
const char*
DiskGetOpcodeName(
    _In_ UCHAR Opcode
)
{
    switch (Opcode) {
    case SCSIOP_TEST_UNIT_READY:
        return "TEST_UNIT_READY";
    case SCSIOP_REQUEST_SENSE:
        return "REQUEST_SENSE";
    case SCSIOP_READ_CAPACITY:
        return "READ_CAPACITY10";
    case SCSIOP_READ_CAPACITY16:
        return "READ_CAPACITY16";
    case SCSIOP_MODE_SENSE:
        return "MODE_SENSE6";
    case SCSIOP_MODE_SENSE10:
        return "MODE_SENSE10";
    case SCSIOP_VERIFY:
        return "VERIFY10";
    case SCSIOP_VERIFY16:
        return "VERIFY16";
    case SCSIOP_READ6:
        return "READ6";
    case SCSIOP_READ:
        return "READ10";
    case SCSIOP_READ12:
        return "READ12";
    case SCSIOP_READ16:
        return "READ16";
    case SCSIOP_WRITE6:
        return "WRITE6";
    case SCSIOP_WRITE:
        return "WRITE10";
    case SCSIOP_WRITE12:
        return "WRITE12";
    case SCSIOP_WRITE16:
        return "WRITE16";
    default:
        return "UNKNOWN";
    }
}

static
VOID
DiskTraceReturnedDataChangedUa(
    _In_ UCHAR TargetId,
    _In_ UCHAR Opcode,
    _In_ const char* Path
)
{
    DbgPrintEx(
        DPFLTR_IHVDRIVER_ID,
        DPFLTR_INFO_LEVEL,
        DRIVER_NAME ": ua returned target=%u opcode=0x%02X(%s) path=%s sense_key=0x06 asc=0x28 ascq=0x00 scsi_status=0x%02X(CHECK_CONDITION)\n",
        (ULONG)TargetId,
        (ULONG)Opcode,
        DiskGetOpcodeName(Opcode),
        Path,
        (ULONG)SCSISTAT_CHECK_CONDITION);
}

static
VOID
DiskFillWriteProtectedSense(
    _Inout_updates_bytes_opt_(*SenseInfoBufferLength) PUCHAR SenseInfoBuffer,
    _Inout_ UCHAR* SenseInfoBufferLength
)
{
    UCHAR senseLength;

    if (SenseInfoBuffer == NULL || SenseInfoBufferLength == NULL || *SenseInfoBufferLength == 0) {
        return;
    }

    senseLength = min(*SenseInfoBufferLength, 18u);
    RtlZeroMemory(SenseInfoBuffer, senseLength);
    SenseInfoBuffer[0] = 0x70;
    if (senseLength > 2) {
        SenseInfoBuffer[2] = SCSI_SENSE_DATA_PROTECT;
    }
    if (senseLength > 7) {
        SenseInfoBuffer[7] = (UCHAR)(senseLength - 8);
    }
    if (senseLength > 12) {
        SenseInfoBuffer[12] = 0x27;
    }
    if (senseLength > 13) {
        SenseInfoBuffer[13] = 0x00;
    }

    *SenseInfoBufferLength = senseLength;
}

static
VOID
DiskFillUnitAttentionSenseDataChanged(
    _Inout_updates_bytes_opt_(*SenseInfoBufferLength) PUCHAR SenseInfoBuffer,
    _Inout_ UCHAR* SenseInfoBufferLength
)
{
    UCHAR senseLength;

    if (SenseInfoBuffer == NULL || SenseInfoBufferLength == NULL || *SenseInfoBufferLength == 0) {
        return;
    }

    senseLength = min(*SenseInfoBufferLength, 18u);
    RtlZeroMemory(SenseInfoBuffer, senseLength);
    SenseInfoBuffer[0] = 0x70;
    if (senseLength > 2) {
        SenseInfoBuffer[2] = SCSI_SENSE_UNIT_ATTENTION;
    }
    if (senseLength > 7) {
        SenseInfoBuffer[7] = (UCHAR)(senseLength - 8);
    }
    if (senseLength > 12) {
        SenseInfoBuffer[12] = 0x28;
    }
    if (senseLength > 13) {
        SenseInfoBuffer[13] = 0x00;
    }

    *SenseInfoBufferLength = senseLength;
}

static
BOOLEAN
DiskTryReturnPendingDataChangedUa(
    _Inout_ PYUME_DISK Disk,
    _In_ UCHAR TargetId,
    _In_ UCHAR Opcode,
    _In_ const char* Path,
    _Inout_ UCHAR* SrbStatus,
    _Inout_ UCHAR* ScsiStatus,
    _Inout_ ULONG* DataTransferLength,
    _Inout_updates_bytes_opt_(*SenseInfoBufferLength) PUCHAR SenseInfoBuffer,
    _Inout_ UCHAR* SenseInfoBufferLength
)
{
    if (!DiskTryConsumePendingDataChangedUa(Disk)) {
        return FALSE;
    }

    *SrbStatus = SRB_STATUS_ERROR;
    *ScsiStatus = SCSISTAT_CHECK_CONDITION;
    *DataTransferLength = 0;
    DiskFillUnitAttentionSenseDataChanged(SenseInfoBuffer, SenseInfoBufferLength);
    DiskTraceReturnedDataChangedUa(TargetId, Opcode, Path);
    return TRUE;
}

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
    _Inout_ PYUME_DISK Disk,
    _In_ UCHAR TargetId,
    _Inout_updates_bytes_(TransferLength) PUCHAR DataBuffer,
    _In_ ULONG TransferLength,
    _Out_ ULONG* DataTransferLength
)
{
    ULONG senseLength;

    senseLength = min(TransferLength, 18u);
    if (senseLength != 0) {
        RtlZeroMemory(DataBuffer, senseLength);
        if (DiskTryConsumePendingDataChangedUa(Disk)) {
            UCHAR fixedSenseLength;

            fixedSenseLength = (UCHAR)senseLength;
            DiskFillUnitAttentionSenseDataChanged(DataBuffer, &fixedSenseLength);
            senseLength = fixedSenseLength;
            DiskTraceReturnedDataChangedUa(TargetId, SCSIOP_REQUEST_SENSE, "request_sense");
        } else {
            DataBuffer[0] = 0x70;
            if (senseLength > 7) {
                DataBuffer[7] = (UCHAR)(senseLength - 8);
            }
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
    _In_ UCHAR DeviceSpecificParameterIndex,
    _In_ BOOLEAN ReadOnly,
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
    if (ReadOnly && DeviceSpecificParameterIndex < RequiredLength) {
        DataBuffer[DeviceSpecificParameterIndex] = 0x80;
    }
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
    UINT64 lba;
    ULONG blockCount;
    NTSTATUS status;

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

    switch (Cdb->CDB6GENERIC.OperationCode) {
    case SCSIOP_REPORT_LUNS:
        DiskHandleReportLuns(DataBuffer, transferLength, DataTransferLength, SrbStatus);
        break;
    case SCSIOP_INQUIRY:
        DiskHandleInquiry(DataBuffer, transferLength, DataTransferLength, SrbStatus);
        DiskRegisterTargetAsyncNotifications(DeviceExtension, TargetId);
        break;
    case SCSIOP_READ_CAPACITY:
        if (DiskTryReturnPendingDataChangedUa(
            disk,
            TargetId,
            Cdb->CDB6GENERIC.OperationCode,
            "pre-check",
            SrbStatus,
            ScsiStatus,
            DataTransferLength,
            SenseInfoBuffer,
            SenseInfoBufferLength)) {
            break;
        }
        DiskHandleReadCapacity10(disk, DataBuffer, transferLength, DataTransferLength, SrbStatus);
        break;
    case SCSIOP_READ_CAPACITY16:
        if (DiskTryReturnPendingDataChangedUa(
            disk,
            TargetId,
            Cdb->CDB6GENERIC.OperationCode,
            "pre-check",
            SrbStatus,
            ScsiStatus,
            DataTransferLength,
            SenseInfoBuffer,
            SenseInfoBufferLength)) {
            break;
        }
        DiskHandleReadCapacity16(disk, Cdb, DataBuffer, transferLength, DataTransferLength, SrbStatus);
        break;
    case SCSIOP_TEST_UNIT_READY:
        DiskTryReturnPendingDataChangedUa(
            disk,
            TargetId,
            Cdb->CDB6GENERIC.OperationCode,
            "pre-check",
            SrbStatus,
            ScsiStatus,
            DataTransferLength,
            SenseInfoBuffer,
            SenseInfoBufferLength);
        break;
    case SCSIOP_REQUEST_SENSE:
        DiskHandleRequestSense(disk, TargetId, DataBuffer, transferLength, DataTransferLength);
        break;
    case SCSIOP_MODE_SENSE:
        if (DiskTryReturnPendingDataChangedUa(
            disk,
            TargetId,
            Cdb->CDB6GENERIC.OperationCode,
            "pre-check",
            SrbStatus,
            ScsiStatus,
            DataTransferLength,
            SenseInfoBuffer,
            SenseInfoBufferLength)) {
            break;
        }
        DiskHandleModeSense(
            DataBuffer,
            transferLength,
            4,
            0,
            3,
            2,
            disk->ReadOnly,
            DataTransferLength,
            SrbStatus);
        break;
    case SCSIOP_MODE_SENSE10:
        if (DiskTryReturnPendingDataChangedUa(
            disk,
            TargetId,
            Cdb->CDB6GENERIC.OperationCode,
            "pre-check",
            SrbStatus,
            ScsiStatus,
            DataTransferLength,
            SenseInfoBuffer,
            SenseInfoBufferLength)) {
            break;
        }
        DiskHandleModeSense(
            DataBuffer,
            transferLength,
            8,
            1,
            6,
            3,
            disk->ReadOnly,
            DataTransferLength,
            SrbStatus);
        break;
    case SCSIOP_MEDIUM_REMOVAL:
    case SCSIOP_START_STOP_UNIT:
    case SCSIOP_SYNCHRONIZE_CACHE:
        break;
    case SCSIOP_VERIFY:
    case SCSIOP_VERIFY16:
        DiskTryReturnPendingDataChangedUa(
            disk,
            TargetId,
            Cdb->CDB6GENERIC.OperationCode,
            "pre-check",
            SrbStatus,
            ScsiStatus,
            DataTransferLength,
            SenseInfoBuffer,
            SenseInfoBufferLength);
        break;
    case SCSIOP_READ6:
    case SCSIOP_READ:
    case SCSIOP_READ12:
    case SCSIOP_READ16:
        if (DiskTryReturnPendingDataChangedUa(
            disk,
            TargetId,
            Cdb->CDB6GENERIC.OperationCode,
            "pre-check",
            SrbStatus,
            ScsiStatus,
            DataTransferLength,
            SenseInfoBuffer,
            SenseInfoBufferLength)) {
            break;
        }
        if (!DiskTryParseReadWriteCdb(disk, Cdb, transferLength, &lba, &blockCount)) {
            *SrbStatus = SRB_STATUS_INVALID_REQUEST;
            *ScsiStatus = SCSISTAT_CHECK_CONDITION;
            *DataTransferLength = 0;
            break;
        }

        status = DiskQueueReadSrb(DeviceExtension, Srb, TargetId, lba, blockCount, transferLength);
        if (status == STATUS_PENDING) {
            *SrbStatus = SRB_STATUS_PENDING;
            return;
        }

        if (!NT_SUCCESS(status)) {
            *SrbStatus = DiskMapQueueFailureToSrbStatus(status);
            *ScsiStatus = SCSISTAT_CHECK_CONDITION;
            *DataTransferLength = 0;
            break;
        }

        *DataTransferLength = transferLength;
        break;
    case SCSIOP_WRITE6:
    case SCSIOP_WRITE:
    case SCSIOP_WRITE12:
    case SCSIOP_WRITE16:
        if (DiskTryReturnPendingDataChangedUa(
            disk,
            TargetId,
            Cdb->CDB6GENERIC.OperationCode,
            "pre-check",
            SrbStatus,
            ScsiStatus,
            DataTransferLength,
            SenseInfoBuffer,
            SenseInfoBufferLength)) {
            break;
        }
        if (disk->ReadOnly) {
            *SrbStatus = SRB_STATUS_ERROR;
            *ScsiStatus = SCSISTAT_CHECK_CONDITION;
            *DataTransferLength = 0;
            DiskFillWriteProtectedSense(SenseInfoBuffer, SenseInfoBufferLength);
            break;
        }

        if (!DiskTryParseReadWriteCdb(disk, Cdb, transferLength, &lba, &blockCount)) {
            *SrbStatus = SRB_STATUS_INVALID_REQUEST;
            *ScsiStatus = SCSISTAT_CHECK_CONDITION;
            *DataTransferLength = 0;
            break;
        }

        status = DiskQueueWriteSrb(DeviceExtension, Srb, TargetId, lba, blockCount, transferLength);
        if (status == STATUS_PENDING) {
            *SrbStatus = SRB_STATUS_PENDING;
            return;
        }

        if (!NT_SUCCESS(status)) {
            *SrbStatus = DiskMapQueueFailureToSrbStatus(status);
            *ScsiStatus = SCSISTAT_CHECK_CONDITION;
            *DataTransferLength = 0;
            break;
        }

        *DataTransferLength = transferLength;
        break;
    default:
        *DataTransferLength = 0;
        *SrbStatus = SRB_STATUS_INVALID_REQUEST;
        break;
    }
}

