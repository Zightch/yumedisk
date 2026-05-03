#include "file.h"

#include "..\session\session.h"
#include "..\transport\transport.h"

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
    YD_KMDF_LOG("FileCreate enter, fileObject=%p", FileObject);
    status = ControlSessionTryOpen(context, FileObject, NULL);
    if (!NT_SUCCESS(status)) {
        YD_KMDF_ERR("ControlSessionTryOpen failed, status=0x%08X", status);
    }
    if (NT_SUCCESS(status)) {
        YD_KMDF_LOG(
            "FileCreate session ready (transport deferred), sessionId=%I64u",
            context->SessionId);
    } else {
        (VOID)ControlSessionClose(context, FileObject);
    }
    YD_KMDF_LOG("FileCreate leave, status=0x%08X", status);
    WdfRequestComplete(Request, status);
}

VOID
ControlEvtFileCleanup(
    _In_ WDFFILEOBJECT FileObject
)
{
    WDFDEVICE device;
    PCTRL_DEVICE_CONTEXT context;
    UINT64 sessionId;

    device = WdfFileObjectGetDevice(FileObject);
    context = ControlGetContext(device);
    sessionId = ControlSessionClose(context, FileObject);
    YD_KMDF_LOG("FileCleanup, fileObject=%p, sessionId=%I64u", FileObject, sessionId);
    if (sessionId != 0) {
        ControlSendSessionCleanup(context, sessionId);
        ControlTransportCloseSession(context);
    }
}

VOID
ControlEvtFileClose(
    _In_ WDFFILEOBJECT FileObject
)
{
    UNREFERENCED_PARAMETER(FileObject);
}

