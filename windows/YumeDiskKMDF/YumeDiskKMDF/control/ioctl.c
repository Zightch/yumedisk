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
    if (message->Header.Size < (ULONG)YUMEDISK_MESSAGE_BASE_SIZE ||
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
    message->Header.Command = Command;
    message->Header.Status = Status;
    message->Header.Reserved0 = 0u;
    message->Header.SessionId = SessionId;
    if (Header != NULL) {
        message->Header.TxId = Header->TxId;
        message->Header.TargetId = Header->TargetId;
        message->Header.Flags = Header->Flags;
    }
    message->Header.Reserved1 = 0u;
}

static
NTSTATUS
ControlHandleQueryKmdfInfo(
    _In_ WDFREQUEST Request,
    _In_ const CTRL_INPUT_MESSAGE* InputMessage,
    _In_ size_t OutputBufferLength,
    _In_ UINT64 SessionId
)
{
    NTSTATUS status;
    PUCHAR outputBuffer;
    size_t outputSize;
    PYUMEDISK_MESSAGE message;
    PYUMEDISK_KMDF_INFO info;

    if (InputMessage->Message->Header.PayloadLength != 0) {
        return STATUS_INVALID_PARAMETER;
    }

    outputBuffer = NULL;
    outputSize = 0;
    status = ControlGetOutputBuffer(
        Request,
        OutputBufferLength,
        YUMEDISK_MESSAGE_BASE_SIZE + sizeof(YUMEDISK_KMDF_INFO),
        &outputBuffer,
        &outputSize);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    RtlZeroMemory(outputBuffer, YUMEDISK_MESSAGE_BASE_SIZE + sizeof(YUMEDISK_KMDF_INFO));
    message = (PYUMEDISK_MESSAGE)outputBuffer;
    ControlInitOutputMessage(
        outputBuffer,
        outputSize,
        &InputMessage->Message->Header,
        YumeDiskCommandQueryKmdfInfo,
        SessionId,
        STATUS_SUCCESS);
    message->Header.PayloadLength = sizeof(YUMEDISK_KMDF_INFO);
    message->Header.Size = YUMEDISK_MESSAGE_BASE_SIZE + sizeof(YUMEDISK_KMDF_INFO);

    info = (PYUMEDISK_KMDF_INFO)message->Payload;
    info->VersionBe = YUMEDISK_COMPONENT_VERSION_BE;

    WdfRequestCompleteWithInformation(Request, STATUS_SUCCESS, message->Header.Size);
    return STATUS_SUCCESS;
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
NTSTATUS
ControlValidateWriteAckBatchPayload(
    _In_reads_bytes_(PayloadLength) const UCHAR* Payload,
    _In_ ULONG PayloadLength
)
{
    PYUMEDISK_WRITE_ACK_BATCH batch;
    ULONG expectedLength;

    if (PayloadLength < (ULONG)YUMEDISK_WRITE_ACK_BATCH_BASE_SIZE) {
        return STATUS_INVALID_PARAMETER;
    }

    batch = (PYUMEDISK_WRITE_ACK_BATCH)Payload;
    if (batch->RangeCount == 0 ||
        batch->RangeCount >
            ((MAXULONG - (ULONG)YUMEDISK_WRITE_ACK_BATCH_BASE_SIZE) / (ULONG)sizeof(YUMEDISK_WRITE_ACK_RANGE))) {
        return STATUS_INVALID_PARAMETER;
    }

    expectedLength = (ULONG)YUMEDISK_WRITE_ACK_BATCH_SIZE(batch->RangeCount);
    if (PayloadLength != expectedLength) {
        return STATUS_INVALID_PARAMETER;
    }

    return STATUS_SUCCESS;
}

static
NTSTATUS
ControlValidateCancelSlotPayload(
    _In_reads_bytes_(PayloadLength) const UCHAR* Payload,
    _In_ ULONG PayloadLength
)
{
    PYUMEDISK_CANCEL_SLOT cancelSlot;

    if (PayloadLength != sizeof(YUMEDISK_CANCEL_SLOT)) {
        return STATUS_INVALID_PARAMETER;
    }

    cancelSlot = (PYUMEDISK_CANCEL_SLOT)Payload;
    if (cancelSlot->SlotId == 0 ||
        cancelSlot->TargetId > YUMEDISK_MAX_USABLE_TARGET_ID ||
        (cancelSlot->SlotType != YumeDiskSlotTypeRead &&
            cancelSlot->SlotType != YumeDiskSlotTypeWrite)) {
        return STATUS_INVALID_PARAMETER;
    }

    return STATUS_SUCCESS;
}

static
NTSTATUS
ControlProxyReadAck(
    _In_ PCTRL_FILE_CONTEXT Context,
    _In_ UINT64 SessionId,
    _In_ const YUMEDISK_HEADER* Header,
    _Inout_ PYUMEDISK_READ_ACK ReadAck,
    _In_opt_ PUCHAR DirectBuffer
)
{
    UCHAR buffer[YUMEDISK_MESSAGE_BASE_SIZE + sizeof(YUMEDISK_READ_ACK)];
    PYUMEDISK_MESSAGE message;
    PYUMEDISK_READ_ACK proxyAck;
    ULONG bytesReturned;

    RtlZeroMemory(buffer, sizeof(buffer));
    message = (PYUMEDISK_MESSAGE)buffer;
    message->Header.Size = sizeof(buffer);
    message->Header.Command = YumeDiskCommandReadAck;
    message->Header.SessionId = SessionId;
    message->Header.TargetId = Header->TargetId;
    message->Header.Flags = Header->Flags;
    message->Header.PayloadLength = sizeof(YUMEDISK_READ_ACK);

    proxyAck = (PYUMEDISK_READ_ACK)message->Payload;
    *proxyAck = *ReadAck;
    proxyAck->KernelVa = (UINT64)(ULONG_PTR)DirectBuffer;

    return ControlProxyCommand(Context, buffer, sizeof(buffer), sizeof(buffer), &bytesReturned);
}

static
NTSTATUS
ControlHandlePostReadSlot(
    _In_ PCTRL_FILE_CONTEXT Context,
    _In_ WDFREQUEST Request,
    _In_ const CTRL_INPUT_MESSAGE* InputMessage,
    _In_ size_t OutputBufferLength,
    _In_ UINT64 SessionId
)
{
    NTSTATUS status;
    PUCHAR outputBuffer;
    size_t outputSize;

    if (InputMessage->Message->Header.PayloadLength != 0 ||
        InputMessage->Message->Header.TargetId > YUMEDISK_MAX_USABLE_TARGET_ID) {
        return STATUS_INVALID_PARAMETER;
    }

    outputBuffer = NULL;
    outputSize = 0;
    status = ControlGetOutputBuffer(
        Request,
        OutputBufferLength,
        sizeof(YUMEDISK_READ_SLOT_EVENT),
        &outputBuffer,
        &outputSize);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    return ControlProxySubmitSlotAsync(
        Context,
        Request,
        SessionId,
        InputMessage->Message->Header.TxId,
        InputMessage->Message->Header.TargetId,
        YumeDiskSlotTypeRead,
        outputBuffer,
        outputSize);
}

static
NTSTATUS
ControlHandlePostWriteSlot(
    _In_ PCTRL_FILE_CONTEXT Context,
    _In_ WDFREQUEST Request,
    _In_ const CTRL_INPUT_MESSAGE* InputMessage,
    _In_ size_t OutputBufferLength,
    _In_ UINT64 SessionId
)
{
    NTSTATUS status;
    PUCHAR outputBuffer;
    size_t outputSize;

    if (InputMessage->Message->Header.PayloadLength != 0 ||
        InputMessage->Message->Header.TargetId > YUMEDISK_MAX_USABLE_TARGET_ID) {
        return STATUS_INVALID_PARAMETER;
    }

    outputBuffer = NULL;
    outputSize = 0;
    status = ControlGetOutputBuffer(
        Request,
        OutputBufferLength,
        YUMEDISK_WRITE_SLOT_HEADER_BASE_SIZE,
        &outputBuffer,
        &outputSize);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    return ControlProxySubmitSlotAsync(
        Context,
        Request,
        SessionId,
        InputMessage->Message->Header.TxId,
        InputMessage->Message->Header.TargetId,
        YumeDiskSlotTypeWrite,
        outputBuffer,
        outputSize);
}

static
VOID
ControlHandleReadAck(
    _In_ PCTRL_FILE_CONTEXT Context,
    _In_ WDFREQUEST Request,
    _In_ const CTRL_INPUT_MESSAGE* InputMessage,
    _In_ size_t OutputBufferLength,
    _In_ UINT64 SessionId
)
{
    NTSTATUS status;
    PUCHAR outputBuffer;
    size_t outputSize;
    PYUMEDISK_READ_ACK readAck;

    if (InputMessage->Message->Header.PayloadLength != sizeof(YUMEDISK_READ_ACK)) {
        WdfRequestComplete(Request, STATUS_INVALID_PARAMETER);
        return;
    }

    readAck = (PYUMEDISK_READ_ACK)InputMessage->Message->Payload;
    if (readAck->EventId == 0 ||
        readAck->KernelVa != 0 ||
        InputMessage->Message->Header.TargetId > YUMEDISK_MAX_USABLE_TARGET_ID) {
        WdfRequestComplete(Request, STATUS_INVALID_PARAMETER);
        return;
    }

    outputBuffer = NULL;
    outputSize = 0;
    if (readAck->DataLength != 0) {
        status = ControlGetOutputBuffer(
            Request,
            OutputBufferLength,
            readAck->DataLength,
            &outputBuffer,
            &outputSize);
        if (!NT_SUCCESS(status)) {
            WdfRequestComplete(Request, status);
            return;
        }

        if (readAck->DataLength > outputSize) {
            WdfRequestComplete(Request, STATUS_BUFFER_TOO_SMALL);
            return;
        }
    }

    status = ControlProxyReadAck(
        Context,
        SessionId,
        &InputMessage->Message->Header,
        readAck,
        outputBuffer);
    WdfRequestCompleteWithInformation(Request, status, 0);
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
    ULONG command;

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

    command = inputMessage.Message->Header.Command;

    if (command == YumeDiskCommandHeartbeat) {
        status = ControlSessionHeartbeat(fileObject, &sessionId);
        if (!NT_SUCCESS(status)) {
            WdfRequestComplete(Request, status);
            return;
        }

        ControlHandleHeartbeat(Request, &inputMessage, OutputBufferLength, sessionId);
        return;
    }

    if (command == YumeDiskCommandPostReadSlot) {
        sessionContext = NULL;
        status = ControlSessionAcquireSlot(fileObject, &sessionContext, &sessionId);
        if (!NT_SUCCESS(status)) {
            WdfRequestComplete(Request, status);
            return;
        }

        status = ControlHandlePostReadSlot(sessionContext, Request, &inputMessage, OutputBufferLength, sessionId);
        if (!NT_SUCCESS(status) && status != STATUS_PENDING) {
            ControlSessionReleaseSlot(sessionContext);
            WdfRequestComplete(Request, status);
        }
        return;
    }

    if (command == YumeDiskCommandPostWriteSlot) {
        sessionContext = NULL;
        status = ControlSessionAcquireSlot(fileObject, &sessionContext, &sessionId);
        if (!NT_SUCCESS(status)) {
            WdfRequestComplete(Request, status);
            return;
        }

        status = ControlHandlePostWriteSlot(sessionContext, Request, &inputMessage, OutputBufferLength, sessionId);
        if (!NT_SUCCESS(status) && status != STATUS_PENDING) {
            ControlSessionReleaseSlot(sessionContext);
            WdfRequestComplete(Request, status);
        }
        return;
    }

    sessionContext = NULL;
    status = ControlSessionAcquire(fileObject, &sessionContext, &sessionId);
    if (!NT_SUCCESS(status)) {
        WdfRequestComplete(Request, status);
        return;
    }

    switch (command) {
    case YumeDiskCommandQueryKmdfInfo:
        status = ControlHandleQueryKmdfInfo(Request, &inputMessage, OutputBufferLength, sessionId);
        if (!NT_SUCCESS(status)) {
            WdfRequestComplete(Request, status);
        }
        ControlSessionRelease(sessionContext);
        return;
    case YumeDiskCommandQueryScsiInfo:
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
    case YumeDiskCommandWriteAckBatch:
        status = ControlValidateWriteAckBatchPayload(
            inputMessage.Message->Payload,
            inputMessage.Message->Header.PayloadLength);
        if (!NT_SUCCESS(status)) {
            ControlSessionRelease(sessionContext);
            WdfRequestComplete(Request, status);
            return;
        }

        status = ControlProxyMessage(sessionContext, Request, &inputMessage, OutputBufferLength, sessionId);
        if (!NT_SUCCESS(status)) {
            WdfRequestComplete(Request, status);
        }
        ControlSessionRelease(sessionContext);
        return;
    case YumeDiskCommandCancelSlot:
        status = ControlValidateCancelSlotPayload(
            inputMessage.Message->Payload,
            inputMessage.Message->Header.PayloadLength);
        if (!NT_SUCCESS(status)) {
            ControlSessionRelease(sessionContext);
            WdfRequestComplete(Request, status);
            return;
        }

        status = ControlProxyMessage(sessionContext, Request, &inputMessage, OutputBufferLength, sessionId);
        if (!NT_SUCCESS(status)) {
            WdfRequestComplete(Request, status);
        }
        ControlSessionRelease(sessionContext);
        return;
    case YumeDiskCommandReadAck:
        ControlHandleReadAck(sessionContext, Request, &inputMessage, OutputBufferLength, sessionId);
        ControlSessionRelease(sessionContext);
        return;
    default:
        ControlSessionRelease(sessionContext);
        WdfRequestComplete(Request, STATUS_INVALID_DEVICE_REQUEST);
        return;
    }
}
