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
        WdfRequestComplete(Request, STATUS_INVALID_DEVICE_REQUEST);
        return;
    }

    if (OutputBufferLength < YUMEDISK_MESSAGE_BASE_SIZE) {
        WdfRequestComplete(Request, STATUS_BUFFER_TOO_SMALL);
        return;
    }

    status = WdfRequestRetrieveOutputBuffer(Request, OutputBufferLength, (PVOID*)&outputBuffer, &outputSize);
    if (!NT_SUCCESS(status)) {
        WdfRequestComplete(Request, status);
        return;
    }

    message = (PYUMEDISK_MESSAGE)outputBuffer;
    if (message->Header.PayloadLength > MAXULONG - (ULONG)YUMEDISK_MESSAGE_BASE_SIZE) {
        WdfRequestComplete(Request, STATUS_INVALID_PARAMETER);
        return;
    }

    requestLength = (ULONG)YUMEDISK_MESSAGE_BASE_SIZE + message->Header.PayloadLength;
    if (message->Header.Version != YUMEDISK_PROTOCOL_VERSION ||
        message->Header.Size < (ULONG)YUMEDISK_MESSAGE_BASE_SIZE ||
        message->Header.Size > outputSize ||
        message->Header.Size < requestLength) {
        WdfRequestComplete(Request, STATUS_INVALID_PARAMETER);
        return;
    }
    bufferCapacity = message->Header.Size;

    sessionId = ControlSessionGetActiveId(context);
    if (sessionId == 0) {
        WdfRequestComplete(Request, STATUS_DEVICE_NOT_READY);
        return;
    }

    message->Header.SessionId = sessionId;
    status = ControlProxyCommand(context, outputBuffer, requestLength, bufferCapacity, &bytesReturned);
    if (bytesReturned == 0) {
        bytesReturned = YUMEDISK_MESSAGE_BASE_SIZE;
    }

    if (bytesReturned >= (ULONG)YUMEDISK_MESSAGE_BASE_SIZE) {
        message->Header.SessionId = sessionId;
    }

    WdfRequestCompleteWithInformation(Request, status, bytesReturned);
}

