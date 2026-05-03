#include "ioctl.h"

#include "..\session\session.h"
#include "..\transport\transport.h"

VOID
ControlEvtIoDeviceControl(
    _In_ WDFQUEUE Queue,
    _In_ WDFREQUEST Request,
    _In_ size_t OutputBufferLength,
    _In_ size_t InputBufferLength,
    _In_ ULONG IoControlCode
)
{
    WDFDEVICE device;
    PCTRL_DEVICE_CONTEXT context;
    PUCHAR outputBuffer;
    size_t outputSize;
    NTSTATUS status;
    ULONG bytesReturned;
    ULONG bufferCapacity;
    ULONG requestLength;
    UINT64 sessionId;
    PYUMEDISK_MESSAGE message;

    UNREFERENCED_PARAMETER(InputBufferLength);

    device = WdfIoQueueGetDevice(Queue);
    context = ControlGetContext(device);
    outputBuffer = NULL;
    bytesReturned = 0;

    if (IoControlCode != IOCTL_YUMEDISK_APP_COMMAND) {
        YD_KMDF_ERR("Ioctl invalid code=0x%08X", IoControlCode);
        WdfRequestComplete(Request, STATUS_INVALID_DEVICE_REQUEST);
        return;
    }

    if (OutputBufferLength < YUMEDISK_MESSAGE_BASE_SIZE) {
        YD_KMDF_ERR("Ioctl output buffer too small, size=%Iu", OutputBufferLength);
        WdfRequestComplete(Request, STATUS_BUFFER_TOO_SMALL);
        return;
    }

    status = WdfRequestRetrieveOutputBuffer(Request, OutputBufferLength, (PVOID*)&outputBuffer, &outputSize);
    if (!NT_SUCCESS(status)) {
        YD_KMDF_ERR("WdfRequestRetrieveOutputBuffer failed, status=0x%08X", status);
        WdfRequestComplete(Request, status);
        return;
    }

    message = (PYUMEDISK_MESSAGE)outputBuffer;
    if (message->Header.PayloadLength > MAXULONG - (ULONG)YUMEDISK_MESSAGE_BASE_SIZE) {
        YD_KMDF_ERR("Ioctl payload overflow, payload=%lu", message->Header.PayloadLength);
        WdfRequestComplete(Request, STATUS_INVALID_PARAMETER);
        return;
    }

    requestLength = (ULONG)YUMEDISK_MESSAGE_BASE_SIZE + message->Header.PayloadLength;
    if (message->Header.Version != YUMEDISK_PROTOCOL_VERSION ||
        message->Header.Size < (ULONG)YUMEDISK_MESSAGE_BASE_SIZE ||
        message->Header.Size > outputSize ||
        message->Header.Size < requestLength) {
        YD_KMDF_ERR(
            "Ioctl invalid header, cmd=%lu version=%lu size=%lu outputSize=%Iu requestLength=%lu",
            message->Header.Command,
            message->Header.Version,
            message->Header.Size,
            outputSize,
            requestLength);
        WdfRequestComplete(Request, STATUS_INVALID_PARAMETER);
        return;
    }
    bufferCapacity = message->Header.Size;

    sessionId = ControlSessionGetActiveId(context);
    if (sessionId == 0) {
        YD_KMDF_ERR("Ioctl no active session, cmd=%lu", message->Header.Command);
        WdfRequestComplete(Request, STATUS_DEVICE_NOT_READY);
        return;
    }

    if (!ControlTransportIsOnline(context)) {
        status = ControlTransportOpenSession(context);
        if (!NT_SUCCESS(status)) {
            YD_KMDF_ERR(
                "ControlTransportOpenSession on demand failed, cmd=%lu status=0x%08X",
                message->Header.Command,
                status);
            WdfRequestComplete(Request, status);
            return;
        }
    }

    message->Header.SessionId = sessionId;
    status = ControlProxyCommand(context, outputBuffer, requestLength, bufferCapacity, &bytesReturned);
    if (!NT_SUCCESS(status)) {
        YD_KMDF_ERR("ControlProxyCommand failed, cmd=%lu status=0x%08X", message->Header.Command, status);
    }
    if (bytesReturned == 0) {
        bytesReturned = YUMEDISK_MESSAGE_BASE_SIZE;
    }

    if (bytesReturned >= (ULONG)YUMEDISK_MESSAGE_BASE_SIZE) {
        message->Header.SessionId = sessionId;
    }

    WdfRequestCompleteWithInformation(Request, status, bytesReturned);
}

