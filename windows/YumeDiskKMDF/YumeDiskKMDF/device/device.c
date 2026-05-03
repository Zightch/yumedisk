#include "device.h"

#include "..\control\file.h"
#include "..\control\ioctl.h"
#include "..\session\session.h"
#include "..\transport\transport.h"

NTSTATUS
ControlAddDevice(
    _In_ WDFDRIVER Driver,
    _Inout_ PWDFDEVICE_INIT DeviceInit
)
{
    WDF_OBJECT_ATTRIBUTES attributes;
    WDF_FILEOBJECT_CONFIG fileConfig;
    WDF_PNPPOWER_EVENT_CALLBACKS pnpCallbacks;
    WDF_IO_QUEUE_CONFIG queueConfig;
    WDFDEVICE device;
    PCTRL_DEVICE_CONTEXT context;
    NTSTATUS status;

    UNREFERENCED_PARAMETER(Driver);

    YD_KMDF_LOG("ControlAddDevice enter%s", "");
    WdfDeviceInitSetExclusive(DeviceInit, TRUE);
    WdfDeviceInitSetIoType(DeviceInit, WdfDeviceIoBuffered);

    WDF_FILEOBJECT_CONFIG_INIT(&fileConfig, ControlEvtFileCreate, ControlEvtFileClose, ControlEvtFileCleanup);
    WdfDeviceInitSetFileObjectConfig(DeviceInit, &fileConfig, WDF_NO_OBJECT_ATTRIBUTES);

    WDF_PNPPOWER_EVENT_CALLBACKS_INIT(&pnpCallbacks);
    WdfDeviceInitSetPnpPowerEventCallbacks(DeviceInit, &pnpCallbacks);

    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attributes, CTRL_DEVICE_CONTEXT);
    attributes.SynchronizationScope = WdfSynchronizationScopeNone;
    attributes.ExecutionLevel = WdfExecutionLevelPassive;

    status = WdfDeviceCreate(&DeviceInit, &attributes, &device);
    if (!NT_SUCCESS(status)) {
        YD_KMDF_ERR("WdfDeviceCreate failed, status=0x%08X", status);
        return status;
    }

    context = ControlGetContext(device);
    ControlSessionInitialize(context);
    ControlTransportInitialize(context);

    WDF_OBJECT_ATTRIBUTES_INIT(&attributes);
    attributes.ParentObject = device;
    status = WdfSpinLockCreate(&attributes, &context->OpenLock);
    if (!NT_SUCCESS(status)) {
        YD_KMDF_ERR("WdfSpinLockCreate failed, status=0x%08X", status);
        return status;
    }

    WDF_IO_QUEUE_CONFIG_INIT_DEFAULT_QUEUE(&queueConfig, WdfIoQueueDispatchParallel);
    queueConfig.EvtIoDeviceControl = ControlEvtIoDeviceControl;
    status = WdfIoQueueCreate(device, &queueConfig, WDF_NO_OBJECT_ATTRIBUTES, WDF_NO_HANDLE);
    if (!NT_SUCCESS(status)) {
        YD_KMDF_ERR("WdfIoQueueCreate failed, status=0x%08X", status);
        return status;
    }

    status = WdfDeviceCreateDeviceInterface(device, (LPGUID)&GUID_YUMEDISK_CONTROL, NULL);
    if (!NT_SUCCESS(status)) {
        YD_KMDF_ERR("WdfDeviceCreateDeviceInterface failed, status=0x%08X", status);
        return status;
    }

    YD_KMDF_LOG("ControlAddDevice ready%s", "");
    return status;
}

