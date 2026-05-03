#include <ntddk.h>
#include <storport.h>

#pragma warning(disable: 4100)
#pragma warning(disable: 4189)

#include "define.h"
#include "disk.h"
#include "utils.h"

static
VOID
VirtHwFreeAdapterResources(
    _In_ PVOID DeviceExtension
)
{
    PDEVICE_CONTEXT extension;
    ULONG index;

    extension = (PDEVICE_CONTEXT)DeviceExtension;
    YumeDiskFreeQueuedState(DeviceExtension);
    YumeDiskCompleteAllPending(DeviceExtension, STATUS_DEVICE_DOES_NOT_EXIST);

    for (index = 0; index < extension->MaxTargets; ++index) {
        YumeDiskResetDiskStorage(&extension->Disk[index]);
    }
}

static
BOOLEAN
VirtHwExtractTargetId(
    _In_ PSTORAGE_REQUEST_BLOCK Srb,
    _Out_ UCHAR* TargetId
)
{
    PUCHAR base;
    PSTOR_ADDRESS address;
    PSTOR_ADDR_BTL8 btl8;

    *TargetId = 0;
    if (Srb->AddressOffset == 0) {
        return TRUE;
    }

    if (Srb->AddressOffset >= Srb->SrbLength) {
        return FALSE;
    }

    base = (PUCHAR)Srb;
    address = (PSTOR_ADDRESS)(base + Srb->AddressOffset);
    if (address->Type != STOR_ADDRESS_TYPE_BTL8 ||
        address->AddressLength != STOR_ADDR_BTL8_ADDRESS_LENGTH) {
        return FALSE;
    }

    btl8 = (PSTOR_ADDR_BTL8)address;
    if (btl8->Path != 0 || btl8->Lun != 0) {
        return FALSE;
    }

    *TargetId = btl8->Target;
    return TRUE;
}

static
BOOLEAN
VirtHwHandleExecuteScsi(
    _In_ PVOID DeviceExtension,
    _In_ PSTORAGE_REQUEST_BLOCK Srb
)
{
    UCHAR targetId;
    PUCHAR base;
    ULONG offset;
    PSRBEX_DATA exData;
    PSRBEX_DATA_SCSI_CDB16 cdb16;

    Srb->SrbStatus = SRB_STATUS_SUCCESS;
    if (!VirtHwExtractTargetId(Srb, &targetId)) {
        Srb->SrbStatus = SRB_STATUS_INVALID_REQUEST;
        StorPortNotification(RequestComplete, DeviceExtension, Srb);
        return TRUE;
    }

    if (Srb->NumSrbExData != 1 || Srb->SrbExDataOffset == NULL) {
        Srb->SrbStatus = SRB_STATUS_INVALID_REQUEST;
        StorPortNotification(RequestComplete, DeviceExtension, Srb);
        return TRUE;
    }

    base = (PUCHAR)Srb;
    offset = Srb->SrbExDataOffset[0];
    if (offset == 0 || offset >= Srb->SrbLength) {
        Srb->SrbStatus = SRB_STATUS_INVALID_REQUEST;
        StorPortNotification(RequestComplete, DeviceExtension, Srb);
        return TRUE;
    }

    exData = (PSRBEX_DATA)(base + offset);
    if (exData->Type != SrbExDataTypeScsiCdb16) {
        Srb->SrbStatus = SRB_STATUS_INVALID_REQUEST;
        StorPortNotification(RequestComplete, DeviceExtension, Srb);
        return TRUE;
    }

    cdb16 = (PSRBEX_DATA_SCSI_CDB16)exData;
    cdb16->ScsiStatus = SCSISTAT_GOOD;

    YumeDiskHandleScsiCdb(
        DeviceExtension,
        Srb,
        targetId,
        &Srb->SrbStatus,
        &cdb16->ScsiStatus,
        (PUCHAR)Srb->DataBuffer,
        &Srb->DataTransferLength,
        (PUCHAR)cdb16->SenseInfoBuffer,
        &cdb16->SenseInfoBufferLength,
        (PCDB)cdb16->Cdb
    );

    if (Srb->SrbStatus != SRB_STATUS_PENDING) {
        StorPortNotification(RequestComplete, DeviceExtension, Srb);
    }

    return TRUE;
}

static
BOOLEAN
VirtHwStartStorageRequest(
    _In_ PVOID DeviceExtension,
    _In_ PSTORAGE_REQUEST_BLOCK Srb
)
{
    if (Srb->SrbFunction == SRB_FUNCTION_EXECUTE_SCSI) {
        return VirtHwHandleExecuteScsi(DeviceExtension, Srb);
    }

    if (Srb->SrbFunction == SRB_FUNCTION_IO_CONTROL) {
        return YumeDiskHandleIoControlSrb(DeviceExtension, Srb);
    }

    Srb->SrbStatus = SRB_STATUS_INVALID_REQUEST;
    StorPortNotification(RequestComplete, DeviceExtension, Srb);
    return TRUE;
}

static
ULONG
VirtHwFindAdapter(
    _In_ PVOID DeviceExtension,
    _In_ PVOID HwContext,
    _In_ PVOID BusInformation,
    _In_ PVOID LowerDevice,
    _In_ PCHAR ArgumentString,
    _Inout_ PPORT_CONFIGURATION_INFORMATION ConfigInfo,
    _In_ PBOOLEAN Again
)
{
    ConfigInfo->WmiDataProvider = FALSE;
    ConfigInfo->VirtualDevice = TRUE;
    ConfigInfo->MaximumNumberOfTargets = YUMEDISK_MAX_TARGETS;
    ConfigInfo->MaximumNumberOfLogicalUnits = 1;
    ConfigInfo->SynchronizationModel = StorSynchronizeFullDuplex;
    ConfigInfo->CachesData = FALSE;
    ConfigInfo->SrbType = SRB_TYPE_STORAGE_REQUEST_BLOCK;
    ConfigInfo->AlignmentMask = FILE_QUAD_ALIGNMENT;
    ConfigInfo->NumberOfBuses = 1;
    ConfigInfo->MapBuffers = STOR_MAP_ALL_BUFFERS_INCLUDING_READ_WRITE;
    return SP_RETURN_FOUND;
}

static
BOOLEAN
VirtHwInitialize(
    _In_ PVOID DeviceExtension
)
{
    PDEVICE_CONTEXT extension;
    ULONG index;

    extension = (PDEVICE_CONTEXT)DeviceExtension;
    RtlZeroMemory(extension, sizeof(*extension));

    KeInitializeSpinLock(&extension->ControlLock);
    InitializeListHead(&extension->PendingEvents);
    InitializeListHead(&extension->PendingWaiters);
    InitializeListHead(&extension->PendingIo);
    extension->MaxTargets = YUMEDISK_MAX_TARGETS;
    extension->NextTxId = 0;

    for (index = 0; index < extension->MaxTargets; ++index) {
        KeInitializeSpinLock(&extension->Disk[index].BufferLock);
        extension->Disk[index].SectorSize = YUMEDISK_DEFAULT_SECTOR_SIZE;
    }

    return TRUE;
}

static
BOOLEAN
VirtHwStartIo(
    _In_ PVOID DeviceExtension,
    _In_ PSCSI_REQUEST_BLOCK Srb
)
{
    if (Srb->Function != SRB_FUNCTION_STORAGE_REQUEST_BLOCK) {
        Srb->SrbStatus = SRB_STATUS_INVALID_REQUEST;
        StorPortNotification(RequestComplete, DeviceExtension, Srb);
        return TRUE;
    }

    return VirtHwStartStorageRequest(DeviceExtension, (PSTORAGE_REQUEST_BLOCK)Srb);
}

static
BOOLEAN
VirtHwResetBus(
    _In_ PVOID DeviceExtension,
    _In_ ULONG PathId
)
{
    return TRUE;
}

static
SCSI_ADAPTER_CONTROL_STATUS
VirtHwAdapterControl(
    _In_ PVOID DeviceExtension,
    _In_ SCSI_ADAPTER_CONTROL_TYPE ControlType,
    _In_ PVOID Parameters
)
{
    switch (ControlType) {
    case ScsiQuerySupportedControlTypes:
    {
        PSCSI_SUPPORTED_CONTROL_TYPE_LIST list;

        list = (PSCSI_SUPPORTED_CONTROL_TYPE_LIST)Parameters;
        if (ScsiQuerySupportedControlTypes < list->MaxControlType) {
            list->SupportedTypeList[ScsiQuerySupportedControlTypes] = TRUE;
        }
        if (ScsiStopAdapter < list->MaxControlType) {
            list->SupportedTypeList[ScsiStopAdapter] = TRUE;
        }
        if (ScsiRestartAdapter < list->MaxControlType) {
            list->SupportedTypeList[ScsiRestartAdapter] = TRUE;
        }
        if (ScsiAdapterPrepareForBusReScan < list->MaxControlType) {
            list->SupportedTypeList[ScsiAdapterPrepareForBusReScan] = TRUE;
        }
        return ScsiAdapterControlSuccess;
    }
    case ScsiStopAdapter:
    case ScsiRestartAdapter:
    case ScsiAdapterPrepareForBusReScan:
        return ScsiAdapterControlSuccess;
    default:
        return ScsiAdapterControlUnsuccessful;
    }
}

NTSTATUS
DriverEntry(
    _In_ PDRIVER_OBJECT DriverObject,
    _In_ PUNICODE_STRING RegistryPath
)
{
    NTSTATUS status;
    VIRTUAL_HW_INITIALIZATION_DATA hwInitData;

    RtlZeroMemory(&hwInitData, sizeof(hwInitData));
    hwInitData.HwInitializationDataSize = sizeof(hwInitData);
    hwInitData.HwFindAdapter = VirtHwFindAdapter;
    hwInitData.HwInitialize = VirtHwInitialize;
    hwInitData.HwStartIo = VirtHwStartIo;
    hwInitData.HwAdapterControl = VirtHwAdapterControl;
    hwInitData.HwFreeAdapterResources = VirtHwFreeAdapterResources;
    hwInitData.HwResetBus = VirtHwResetBus;
    hwInitData.AdapterInterfaceType = Internal;
    hwInitData.MultipleRequestPerLu = TRUE;
    hwInitData.AutoRequestSense = TRUE;
    hwInitData.DeviceExtensionSize = sizeof(DEVICE_CONTEXT);
    hwInitData.SpecificLuExtensionSize = 0;
    hwInitData.SrbExtensionSize = 0;

    status = StorPortInitialize(DriverObject, RegistryPath, (PHW_INITIALIZATION_DATA)&hwInitData, NULL);
    if (!NT_SUCCESS(status)) {
        DbgPrint("%s StorPortInitialize failed: %08x\n", DRIVER_NAME, status);
    }

    return status;
}
