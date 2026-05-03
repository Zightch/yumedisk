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
    PUCHAR inputBuffer;
    PUCHAR outputBuffer;
    size_t inputSize;
    size_t outputSize;
    size_t maxSize;
    NTSTATUS status;
    ULONG bytesReturned;
    ULONG bufferCapacity;
    UINT64 sessionId;
    PYUMEDISK_MESSAGE message;

    device = WdfIoQueueGetDevice(Queue);
    context = ControlGetContext(device);
    inputBuffer = NULL;
    outputBuffer = NULL;
    bytesReturned = 0;

    if (IoControlCode != IOCTL_YUMEDISK_APP_COMMAND) {
        WdfRequestComplete(Request, STATUS_INVALID_DEVICE_REQUEST);
        return;
    }

    if (InputBufferLength < YUMEDISK_MESSAGE_BASE_SIZE ||
        OutputBufferLength < YUMEDISK_MESSAGE_BASE_SIZE) {
        WdfRequestComplete(Request, STATUS_BUFFER_TOO_SMALL);
        return;
    }

    status = WdfRequestRetrieveInputBuffer(Request, InputBufferLength, (PVOID*)&inputBuffer, &inputSize);
    if (!NT_SUCCESS(status)) {
        WdfRequestComplete(Request, status);
        return;
    }

    status = WdfRequestRetrieveOutputBuffer(Request, OutputBufferLength, (PVOID*)&outputBuffer, &outputSize);
    if (!NT_SUCCESS(status)) {
        WdfRequestComplete(Request, status);
        return;
    }

    maxSize = (inputSize > outputSize) ? inputSize : outputSize;
    bufferCapacity = (ULONG)((maxSize > MAXULONG) ? MAXULONG : maxSize);
    if (inputBuffer != outputBuffer) {
        RtlCopyMemory(outputBuffer, inputBuffer, min(inputSize, outputSize));
    }

    message = (PYUMEDISK_MESSAGE)outputBuffer;
    if (message->Header.Version != YUMEDISK_PROTOCOL_VERSION ||
        message->Header.Size < (ULONG)YUMEDISK_MESSAGE_BASE_SIZE ||
        message->Header.Size > bufferCapacity ||
        message->Header.Size < (ULONG)(YUMEDISK_MESSAGE_BASE_SIZE + message->Header.PayloadLength)) {
        WdfRequestComplete(Request, STATUS_INVALID_PARAMETER);
        return;
    }

    sessionId = ControlSessionGetActiveId(context);
    if (sessionId == 0) {
        WdfRequestComplete(Request, STATUS_DEVICE_NOT_READY);
        return;
    }

    message->Header.SessionId = sessionId;
    status = ControlProxyCommand(outputBuffer, (ULONG)inputSize, bufferCapacity, &bytesReturned);
    if (bytesReturned == 0) {
        bytesReturned = YUMEDISK_MESSAGE_BASE_SIZE;
    }

    if (bytesReturned >= (ULONG)YUMEDISK_MESSAGE_BASE_SIZE) {
        message->Header.SessionId = sessionId;
    }

    WdfRequestCompleteWithInformation(Request, status, bytesReturned);
}

