#include <ntddk.h>
#include <wdf.h>

#include "..\device\device.h"

static
NTSTATUS
ControlEvtDeviceAdd(
    _In_ WDFDRIVER Driver,
    _Inout_ PWDFDEVICE_INIT DeviceInit
)
{
    NTSTATUS status;

    YD_KMDF_LOG("EvtDeviceAdd enter%s", "");
    status = ControlAddDevice(Driver, DeviceInit);
    YD_KMDF_LOG("EvtDeviceAdd leave, status=0x%08X", status);
    return status;
}

NTSTATUS
DriverEntry(
    _In_ PDRIVER_OBJECT DriverObject,
    _In_ PUNICODE_STRING RegistryPath
)
{
    WDF_DRIVER_CONFIG config;
    NTSTATUS status;

    WDF_DRIVER_CONFIG_INIT(&config, ControlEvtDeviceAdd);
    YD_KMDF_LOG("DriverEntry enter, build=%s %s", __DATE__, __TIME__);
    status = WdfDriverCreate(
        DriverObject,
        RegistryPath,
        WDF_NO_OBJECT_ATTRIBUTES,
        &config,
        WDF_NO_HANDLE);
    YD_KMDF_LOG("DriverEntry leave, status=0x%08X", status);
    return status;
}

