#include "adapter.h"
#include <ntddstor.h>

#pragma warning(disable: 4100)
#pragma warning(disable: 4189)

static
VOID
DiskFreeAdapterResources(
    _In_ PVOID DeviceExtension
)
{
    PDEVICE_CONTEXT extension;
    ULONG index;

    extension = (PDEVICE_CONTEXT)DeviceExtension;
    DiskCompleteAllPending(DeviceExtension, STATUS_DEVICE_DOES_NOT_EXIST);
    DiskFreeQueuedState(DeviceExtension);

    for (index = 0; index < extension->MaxTargets; ++index) {
        DiskResetDiskStorage(&extension->Disk[index]);
    }
}

static
BOOLEAN
DiskExtractTargetId(
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
DiskFillQueryCapabilities(
    _Inout_ PSCSI_PNP_REQUEST_BLOCK PnpSrb
)
{
    if (PnpSrb->DataBuffer == NULL ||
        PnpSrb->DataTransferLength < sizeof(STOR_DEVICE_CAPABILITIES)) {
        PnpSrb->SrbStatus = SRB_STATUS_INVALID_REQUEST;
        return FALSE;
    }

    if (PnpSrb->DataTransferLength >= sizeof(STOR_DEVICE_CAPABILITIES_EX)) {
        PSTOR_DEVICE_CAPABILITIES_EX capabilitiesEx;

        capabilitiesEx = (PSTOR_DEVICE_CAPABILITIES_EX)PnpSrb->DataBuffer;
        RtlZeroMemory(capabilitiesEx, sizeof(*capabilitiesEx));
        capabilitiesEx->Version = STOR_DEVICE_CAPABILITIES_EX_VERSION_1;
        capabilitiesEx->Size = (USHORT)sizeof(*capabilitiesEx);
        capabilitiesEx->SilentInstall = 1;
        capabilitiesEx->RawDeviceOK = 1;
        capabilitiesEx->SurpriseRemovalOK = 0;
        capabilitiesEx->NoDisplayInUI = 0;
        return TRUE;
    }

    {
        PSTOR_DEVICE_CAPABILITIES capabilities;

        capabilities = (PSTOR_DEVICE_CAPABILITIES)PnpSrb->DataBuffer;
        RtlZeroMemory(capabilities, sizeof(*capabilities));
        capabilities->Version = STOR_DEVICE_CAPABILITIES_EX_VERSION_1;
        capabilities->SilentInstall = 1;
        capabilities->SurpriseRemovalOK = 0;
        capabilities->NoDisplayInUI = 0;
    }

    return TRUE;
}

static
BOOLEAN
DiskHandleAdapterPnpRequest(
    _In_ PVOID DeviceExtension,
    _In_ PSTORAGE_REQUEST_BLOCK Srb
)
{
    PSCSI_PNP_REQUEST_BLOCK pnpSrb;

    pnpSrb = (PSCSI_PNP_REQUEST_BLOCK)Srb;
    pnpSrb->SrbStatus = SRB_STATUS_SUCCESS;

    switch (pnpSrb->PnPAction) {
    case StorQueryCapabilities:
        (VOID)DiskFillQueryCapabilities(pnpSrb);
        break;
    case StorRemoveDevice:
    case StorSurpriseRemoval:
    case StorStartDevice:
    case StorStopDevice:
    case StorQueryResourceRequirements:
    case StorFilterResourceRequirements:
        break;
    default:
        pnpSrb->SrbStatus = SRB_STATUS_INVALID_REQUEST;
        break;
    }

    StorPortNotification(RequestComplete, DeviceExtension, Srb);
    return TRUE;
}

static
BOOLEAN
DiskHandlePnpRequest(
    _In_ PVOID DeviceExtension,
    _In_ PSTORAGE_REQUEST_BLOCK Srb
)
{
    PSCSI_PNP_REQUEST_BLOCK pnpSrb;

    pnpSrb = (PSCSI_PNP_REQUEST_BLOCK)Srb;
    pnpSrb->SrbStatus = SRB_STATUS_SUCCESS;

    if ((pnpSrb->SrbPnPFlags & SRB_PNP_FLAGS_ADAPTER_REQUEST) != 0) {
        return DiskHandleAdapterPnpRequest(DeviceExtension, Srb);
    }

    if (pnpSrb->PathId != 0 ||
        pnpSrb->Lun != 0 ||
        !DiskIsUsableTargetId(pnpSrb->TargetId)) {
        pnpSrb->SrbStatus = SRB_STATUS_INVALID_REQUEST;
        StorPortNotification(RequestComplete, DeviceExtension, Srb);
        return TRUE;
    }

    switch (pnpSrb->PnPAction) {
    case StorQueryCapabilities:
        (VOID)DiskFillQueryCapabilities(pnpSrb);
        break;
    case StorRemoveDevice:
    case StorSurpriseRemoval:
    case StorStartDevice:
    case StorStopDevice:
    case StorQueryResourceRequirements:
    case StorFilterResourceRequirements:
        break;
    default:
        pnpSrb->SrbStatus = SRB_STATUS_INVALID_REQUEST;
        break;
    }

    StorPortNotification(RequestComplete, DeviceExtension, Srb);
    return TRUE;
}

static
SCSI_UNIT_CONTROL_STATUS
DiskUnitControl(
    _In_ PVOID DeviceExtension,
    _In_ SCSI_UNIT_CONTROL_TYPE ControlType,
    _In_ PVOID Parameters
)
{
    PDEVICE_CONTEXT extension;

    extension = (PDEVICE_CONTEXT)DeviceExtension;
    switch (ControlType) {
    case ScsiQuerySupportedUnitControlTypes:
    {
        PSCSI_SUPPORTED_CONTROL_TYPE_LIST list;

        list = (PSCSI_SUPPORTED_CONTROL_TYPE_LIST)Parameters;
        if (ScsiQuerySupportedUnitControlTypes < list->MaxControlType) {
            list->SupportedTypeList[ScsiQuerySupportedUnitControlTypes] = TRUE;
        }
        if (ScsiUnitQueryBusType < list->MaxControlType) {
            list->SupportedTypeList[ScsiUnitQueryBusType] = TRUE;
        }
        return ScsiUnitControlSuccess;
    }
    case ScsiUnitQueryBusType:
    {
        PSTOR_UNIT_CONTROL_QUERY_BUS_TYPE query;
        PSTOR_ADDR_BTL8 address;

        query = (PSTOR_UNIT_CONTROL_QUERY_BUS_TYPE)Parameters;
        if (query == NULL || query->Address == NULL) {
            return ScsiUnitControlUnsuccessful;
        }

        if (query->Address->Type != STOR_ADDRESS_TYPE_BTL8 ||
            query->Address->AddressLength != STOR_ADDR_BTL8_ADDRESS_LENGTH) {
            return ScsiUnitControlUnsuccessful;
        }

        address = (PSTOR_ADDR_BTL8)query->Address;
        if (address->Path != 0 ||
            address->Lun != 0 ||
            address->Target >= extension->MaxTargets ||
            !DiskIsUsableTargetId(address->Target)) {
            return ScsiUnitControlUnsuccessful;
        }

        query->BusType = BusTypeVirtual;
        return ScsiUnitControlSuccess;
    }
    default:
        return ScsiUnitControlNotSupported;
    }
}

static
BOOLEAN
DiskHandleExecuteScsi(
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
    if (!DiskExtractTargetId(Srb, &targetId)) {
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

    DiskHandleScsiCdb(
        DeviceExtension,
        Srb,
        targetId,
        &Srb->SrbStatus,
        &cdb16->ScsiStatus,
        (PUCHAR)Srb->DataBuffer,
        &Srb->DataTransferLength,
        (PUCHAR)cdb16->SenseInfoBuffer,
        &cdb16->SenseInfoBufferLength,
        (PCDB)cdb16->Cdb);

    if (Srb->SrbStatus != SRB_STATUS_PENDING) {
        StorPortNotification(RequestComplete, DeviceExtension, Srb);
    }

    return TRUE;
}

static
BOOLEAN
DiskStartStorageRequest(
    _In_ PVOID DeviceExtension,
    _In_ PSTORAGE_REQUEST_BLOCK Srb
)
{
    UCHAR targetId;

    switch (Srb->SrbFunction) {
#ifdef SRB_FUNCTION_ABORT_COMMAND
    case SRB_FUNCTION_ABORT_COMMAND:
#endif
#ifdef SRB_FUNCTION_RESET_DEVICE
    case SRB_FUNCTION_RESET_DEVICE:
#endif
#ifdef SRB_FUNCTION_RESET_LOGICAL_UNIT
    case SRB_FUNCTION_RESET_LOGICAL_UNIT:
#endif
#ifdef SRB_FUNCTION_RESET_BUS
    case SRB_FUNCTION_RESET_BUS:
#endif
        if (DiskExtractTargetId(Srb, &targetId)) {
            DiskCompleteTargetPending(DeviceExtension, targetId, STATUS_CANCELLED);
        } else {
            DiskCompleteAllPending(DeviceExtension, STATUS_CANCELLED);
        }
        Srb->SrbStatus = SRB_STATUS_SUCCESS;
        StorPortNotification(RequestComplete, DeviceExtension, Srb);
        return TRUE;
    default:
        break;
    }

    if (Srb->SrbFunction == SRB_FUNCTION_EXECUTE_SCSI) {
        return DiskHandleExecuteScsi(DeviceExtension, Srb);
    }

    if (Srb->SrbFunction == SRB_FUNCTION_IO_CONTROL) {
        return DiskHandleIoControlSrb(DeviceExtension, Srb);
    }

    if (Srb->SrbFunction == SRB_FUNCTION_PNP) {
        return DiskHandlePnpRequest(DeviceExtension, Srb);
    }

    Srb->SrbStatus = SRB_STATUS_INVALID_REQUEST;
    StorPortNotification(RequestComplete, DeviceExtension, Srb);
    return TRUE;
}

static
ULONG
DiskFindAdapter(
    _In_ PVOID DeviceExtension,
    _In_ PVOID HwContext,
    _In_ PVOID BusInformation,
    _In_ PVOID LowerDevice,
    _In_ PCHAR ArgumentString,
    _Inout_ PPORT_CONFIGURATION_INFORMATION ConfigInfo,
    _In_ PBOOLEAN Again
)
{
    BOOLEAN featureList[StorportFeatureMax];

    UNREFERENCED_PARAMETER(DeviceExtension);
    UNREFERENCED_PARAMETER(HwContext);
    UNREFERENCED_PARAMETER(BusInformation);
    UNREFERENCED_PARAMETER(LowerDevice);
    UNREFERENCED_PARAMETER(ArgumentString);
    UNREFERENCED_PARAMETER(Again);

    RtlZeroMemory(featureList, sizeof(featureList));
    featureList[StorportFeatureBusTypeUnitControl] = TRUE;
    (VOID)StorPortSetFeatureList(DeviceExtension, StorportFeatureMax, featureList);
    (VOID)StorPortSetAdapterBusType(DeviceExtension, BusTypeVirtual);

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
DiskInitializeAdapter(
    _In_ PVOID DeviceExtension
)
{
    PDEVICE_CONTEXT extension;
    ULONG index;

    extension = (PDEVICE_CONTEXT)DeviceExtension;
    RtlZeroMemory(extension, sizeof(*extension));

    KeInitializeSpinLock(&extension->SessionLock);
    extension->MaxTargets = YUMEDISK_MAX_TARGETS;
    DiskInitializeQueueState(extension);

    for (index = 0; index < extension->MaxTargets; ++index) {
        KeInitializeSpinLock(&extension->Disk[index].BufferLock);
        KeInitializeSpinLock(&extension->Disk[index].Queue.ReadQueueLock);
        KeInitializeSpinLock(&extension->Disk[index].Queue.WriteQueueLock);
        KeInitializeSpinLock(&extension->Disk[index].EventSlot.Lock);
        extension->Disk[index].SectorSize = YUMEDISK_DEFAULT_SECTOR_SIZE;
    }

    return TRUE;
}

static
BOOLEAN
DiskStartIo(
    _In_ PVOID DeviceExtension,
    _In_ PSCSI_REQUEST_BLOCK Srb
)
{
    if (Srb->Function != SRB_FUNCTION_STORAGE_REQUEST_BLOCK) {
        Srb->SrbStatus = SRB_STATUS_INVALID_REQUEST;
        StorPortNotification(RequestComplete, DeviceExtension, Srb);
        return TRUE;
    }

    return DiskStartStorageRequest(DeviceExtension, (PSTORAGE_REQUEST_BLOCK)Srb);
}

static
BOOLEAN
DiskResetBus(
    _In_ PVOID DeviceExtension,
    _In_ ULONG PathId
)
{
    DiskCompleteAllPending(DeviceExtension, STATUS_CANCELLED);
    UNREFERENCED_PARAMETER(PathId);
    return TRUE;
}

SCSI_ADAPTER_CONTROL_STATUS
DiskAdapterControl(
    _In_ PVOID DeviceExtension,
    _In_ SCSI_ADAPTER_CONTROL_TYPE ControlType,
    _In_ PVOID Parameters
)
{
    UNREFERENCED_PARAMETER(DeviceExtension);

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
DiskDriverEntry(
    _In_ PDRIVER_OBJECT DriverObject,
    _In_ PUNICODE_STRING RegistryPath
)
{
    NTSTATUS status;
    HW_INITIALIZATION_DATA hwInitData;

    RtlZeroMemory(&hwInitData, sizeof(hwInitData));
    hwInitData.HwInitializationDataSize = sizeof(hwInitData);
    hwInitData.HwFindAdapter = (PVOID)DiskFindAdapter;
    hwInitData.HwInitialize = DiskInitializeAdapter;
    hwInitData.HwStartIo = DiskStartIo;
    hwInitData.HwAdapterControl = DiskAdapterControl;
    hwInitData.HwUnitControl = DiskUnitControl;
    hwInitData.HwFreeAdapterResources = DiskFreeAdapterResources;
    hwInitData.HwResetBus = DiskResetBus;
    hwInitData.AdapterInterfaceType = Internal;
    hwInitData.MultipleRequestPerLu = TRUE;
    hwInitData.AutoRequestSense = TRUE;
    hwInitData.DeviceExtensionSize = sizeof(DEVICE_CONTEXT);
    hwInitData.SpecificLuExtensionSize = 0;
    hwInitData.SrbExtensionSize = 0;
    hwInitData.FeatureSupport =
        STOR_FEATURE_VIRTUAL_MINIPORT |
        STOR_FEATURE_FULL_PNP_DEVICE_CAPABILITIES;

    status = StorPortInitialize(
        DriverObject,
        RegistryPath,
        (PHW_INITIALIZATION_DATA)&hwInitData,
        NULL);

    return status;
}

