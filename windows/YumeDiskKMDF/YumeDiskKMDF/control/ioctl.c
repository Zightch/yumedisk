#include "ioctl.h"

#include "..\session\session.h"
#include "..\transport\transport.h"

typedef struct _CTRL_INPUT_MESSAGE {
    PUCHAR Buffer;
    size_t BufferSize;
    PYUMEDISK_MESSAGE Message;
    ULONG RequestLength;
} CTRL_INPUT_MESSAGE, *PCTRL_INPUT_MESSAGE;

static
NTSTATUS
ControlGetInputMessage(
    _In_ WDFREQUEST Request,
    _In_ size_t InputBufferLength,
    _Out_ PCTRL_INPUT_MESSAGE InputMessage
)
{
    NTSTATUS status;
    PUCHAR inputBuffer;
    size_t inputSize;
    PYUMEDISK_MESSAGE message;

    RtlZeroMemory(InputMessage, sizeof(*InputMessage));

    if (InputBufferLength < YUMEDISK_MESSAGE_BASE_SIZE) {
        return STATUS_BUFFER_TOO_SMALL;
    }

    inputBuffer = NULL;
    status = WdfRequestRetrieveInputBuffer(Request, InputBufferLength, (PVOID*)&inputBuffer, &inputSize);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    message = (PYUMEDISK_MESSAGE)inputBuffer;
    if (message->Header.PayloadLength > MAXULONG - (ULONG)YUMEDISK_MESSAGE_BASE_SIZE) {
        return STATUS_INVALID_PARAMETER;
    }

    InputMessage->RequestLength = (ULONG)YUMEDISK_MESSAGE_BASE_SIZE + message->Header.PayloadLength;
    if (message->Header.Version != YUMEDISK_PROTOCOL_VERSION ||
        message->Header.Size < (ULONG)YUMEDISK_MESSAGE_BASE_SIZE ||
        message->Header.Size > inputSize ||
        message->Header.Size < InputMessage->RequestLength) {
        return STATUS_INVALID_PARAMETER;
    }

    InputMessage->Buffer = inputBuffer;
    InputMessage->BufferSize = inputSize;
    InputMessage->Message = message;
    return STATUS_SUCCESS;
}

static
NTSTATUS
ControlGetOutputBuffer(
    _In_ WDFREQUEST Request,
    _In_ size_t OutputBufferLength,
    _In_ size_t MinimumSize,
    _Outptr_result_bytebuffer_(*OutputSize) PUCHAR* OutputBuffer,
    _Out_ size_t* OutputSize
)
{
    if (OutputBufferLength < MinimumSize) {
        return STATUS_BUFFER_TOO_SMALL;
    }

    return WdfRequestRetrieveOutputBuffer(Request, MinimumSize, (PVOID*)OutputBuffer, OutputSize);
}

static
VOID
ControlInitOutputMessage(
    _Out_writes_bytes_(OutputSize) PUCHAR OutputBuffer,
    _In_ size_t OutputSize,
    _In_ const YUMEDISK_HEADER* Header,
    _In_ ULONG Command,
    _In_ UINT64 SessionId,
    _In_ NTSTATUS Status
)
{
    PYUMEDISK_MESSAGE message;

    if (OutputSize < YUMEDISK_MESSAGE_BASE_SIZE) {
        return;
    }

    message = (PYUMEDISK_MESSAGE)OutputBuffer;
    RtlZeroMemory(OutputBuffer, YUMEDISK_MESSAGE_BASE_SIZE);
    message->Header.Size = YUMEDISK_MESSAGE_BASE_SIZE;
    message->Header.Version = YUMEDISK_PROTOCOL_VERSION;
    message->Header.Command = Command;
    message->Header.Status = Status;
    message->Header.SessionId = SessionId;
    if (Header != NULL) {
        message->Header.TxId = Header->TxId;
        message->Header.TargetId = Header->TargetId;
        message->Header.Flags = Header->Flags;
    }
}

static
NTSTATUS
ControlProxyMessage(
    _In_ PCTRL_FILE_CONTEXT Context,
    _In_ WDFREQUEST Request,
    _In_ PCTRL_INPUT_MESSAGE InputMessage,
    _In_ size_t OutputBufferLength,
    _In_ UINT64 SessionId
)
{
    NTSTATUS status;
    PUCHAR outputBuffer;
    size_t outputSize;
    ULONG bytesReturned;

    outputBuffer = NULL;
    outputSize = 0;
    bytesReturned = 0;

    status = ControlGetOutputBuffer(
        Request,
        OutputBufferLength,
        InputMessage->Message->Header.Size,
        &outputBuffer,
        &outputSize);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    RtlZeroMemory(outputBuffer, InputMessage->Message->Header.Size);
    RtlCopyMemory(outputBuffer, InputMessage->Buffer, InputMessage->RequestLength);
    ((PYUMEDISK_MESSAGE)outputBuffer)->Header.SessionId = SessionId;

    status = ControlProxyCommand(
        Context,
        outputBuffer,
        InputMessage->RequestLength,
        InputMessage->Message->Header.Size,
        &bytesReturned);
    if (bytesReturned == 0) {
        bytesReturned = YUMEDISK_MESSAGE_BASE_SIZE;
    }

    if (bytesReturned >= (ULONG)YUMEDISK_MESSAGE_BASE_SIZE) {
        ((PYUMEDISK_MESSAGE)outputBuffer)->Header.SessionId = SessionId;
    }

    WdfRequestCompleteWithInformation(Request, status, bytesReturned);
    return STATUS_SUCCESS;
}

static
VOID
ControlHandleHeartbeat(
    _In_ WDFREQUEST Request,
    _In_ const CTRL_INPUT_MESSAGE* InputMessage,
    _In_ size_t OutputBufferLength,
    _In_ UINT64 SessionId
)
{
    NTSTATUS status;
    PUCHAR outputBuffer;
    size_t outputSize;

    outputBuffer = NULL;
    outputSize = 0;

    status = ControlGetOutputBuffer(
        Request,
        OutputBufferLength,
        YUMEDISK_MESSAGE_BASE_SIZE,
        &outputBuffer,
        &outputSize);
    if (!NT_SUCCESS(status)) {
        WdfRequestComplete(Request, status);
        return;
    }

    ControlInitOutputMessage(
        outputBuffer,
        outputSize,
        &InputMessage->Message->Header,
        YumeDiskCommandHeartbeat,
        SessionId,
        STATUS_SUCCESS);
    WdfRequestCompleteWithInformation(Request, STATUS_SUCCESS, YUMEDISK_MESSAGE_BASE_SIZE);
}

static
VOID
ControlCompleteUnsupportedDataCommand(
    _In_ PCTRL_FILE_CONTEXT SessionContext,
    _In_ WDFREQUEST Request
)
{
    ControlSessionRelease(SessionContext);
    WdfRequestComplete(Request, STATUS_NOT_SUPPORTED);
}

VOID
ControlEvtIoDeviceControl(
    _In_ WDFQUEUE Queue,
    _In_ WDFREQUEST Request,
    _In_ size_t OutputBufferLength,
    _In_ size_t InputBufferLength,
    _In_ ULONG IoControlCode
)
{
    WDFFILEOBJECT fileObject;
    PCTRL_FILE_CONTEXT sessionContext;
    CTRL_INPUT_MESSAGE inputMessage;
    UINT64 sessionId;
    NTSTATUS status;

    UNREFERENCED_PARAMETER(Queue);

    if (IoControlCode != IOCTL_YUMEDISK_APP_COMMAND) {
        WdfRequestComplete(Request, STATUS_INVALID_DEVICE_REQUEST);
        return;
    }

    status = ControlGetInputMessage(Request, InputBufferLength, &inputMessage);
    if (!NT_SUCCESS(status)) {
        WdfRequestComplete(Request, status);
        return;
    }

    fileObject = WdfRequestGetFileObject(Request);
    if (fileObject == NULL) {
        WdfRequestComplete(Request, STATUS_INVALID_DEVICE_REQUEST);
        return;
    }

    if (inputMessage.Message->Header.Command == YumeDiskCommandHeartbeat) {
        status = ControlSessionHeartbeat(fileObject, &sessionId);
        if (!NT_SUCCESS(status)) {
            WdfRequestComplete(Request, status);
            return;
        }

        ControlHandleHeartbeat(Request, &inputMessage, OutputBufferLength, sessionId);
        return;
    }

    sessionContext = NULL;
    status = ControlSessionAcquire(fileObject, &sessionContext, &sessionId);
    if (!NT_SUCCESS(status)) {
        WdfRequestComplete(Request, status);
        return;
    }

    switch (inputMessage.Message->Header.Command) {
    case YumeDiskCommandQueryInfo:
    case YumeDiskCommandQueryDebugState:
    case YumeDiskCommandCreateDisk:
    case YumeDiskCommandRemoveDisk:
    case YumeDiskCommandRemoveAllDisks:
        status = ControlProxyMessage(sessionContext, Request, &inputMessage, OutputBufferLength, sessionId);
        if (!NT_SUCCESS(status)) {
            WdfRequestComplete(Request, status);
        }
        ControlSessionRelease(sessionContext);
        return;
    case YumeDiskCommandPostReadSlot:
    case YumeDiskCommandPostWriteSlot:
    case YumeDiskCommandReadAck:
    case YumeDiskCommandWriteAckBatch:
    case YumeDiskCommandCancelSlot:
        ControlCompleteUnsupportedDataCommand(sessionContext, Request);
        return;
    default:
        ControlSessionRelease(sessionContext);
        WdfRequestComplete(Request, STATUS_INVALID_DEVICE_REQUEST);
        return;
    }
}
