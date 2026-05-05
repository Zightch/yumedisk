#include "file.h"

#include "..\session\session.h"

VOID
ControlEvtFileCreate(
    _In_ WDFDEVICE Device,
    _In_ WDFREQUEST Request,
    _In_ WDFFILEOBJECT FileObject
)
{
    PCTRL_DEVICE_CONTEXT context;
    NTSTATUS status;

    context = ControlGetContext(Device);
    status = ControlSessionTryOpen(context, FileObject, NULL);
    WdfRequestComplete(Request, status);
}

VOID
ControlEvtFileCleanup(
    _In_ WDFFILEOBJECT FileObject
)
{
    WDFDEVICE device;
    PCTRL_DEVICE_CONTEXT context;

    device = WdfFileObjectGetDevice(FileObject);
    context = ControlGetContext(device);
    ControlSessionCleanup(context, FileObject);
}

VOID
ControlEvtFileClose(
    _In_ WDFFILEOBJECT FileObject
)
{
    UNREFERENCED_PARAMETER(FileObject);
}

