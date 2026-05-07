#include "transport.h"

#include <ntddscsi.h>

#include "..\core\memory.h"
#include "..\session\session.h"
#include "runtime.h"

_IRQL_requires_max_(PASSIVE_LEVEL)
NTSYSAPI
NTSTATUS
NTAPI
ZwCreateEvent(
    _Out_ PHANDLE EventHandle,
    _In_ ACCESS_MASK DesiredAccess,
    _In_opt_ POBJECT_ATTRIBUTES ObjectAttributes,
    _In_ EVENT_TYPE EventType,
    _In_ BOOLEAN InitialState
);

_When_(Timeout == NULL, _IRQL_requires_max_(APC_LEVEL))
_When_(Timeout->QuadPart != 0, _IRQL_requires_max_(APC_LEVEL))
_When_(Timeout->QuadPart == 0, _IRQL_requires_max_(DISPATCH_LEVEL))
NTSYSAPI
NTSTATUS
NTAPI
ZwWaitForSingleObject(
    _In_ HANDLE Handle,
    _In_ BOOLEAN Alertable,
    _In_opt_ PLARGE_INTEGER Timeout
);

static const GUID ControlStoragePortGuid = {
    0x2accfe60, 0xc130, 0x11d2, { 0xb0, 0x82, 0x00, 0xa0, 0xc9, 0x1e, 0xfb, 0x8b }
};

#define CTRL_SLOT_TRACE_LIMIT 160

static volatile LONG g_ControlSlotTraceCount = 0;

static
BOOLEAN
ControlShouldTraceSlot(
    _In_ UINT32 SlotType,
    _In_ NTSTATUS Status
)
{
    if (!NT_SUCCESS(Status)) {
        return TRUE;
    }

    if (SlotType != YumeDiskSlotTypeWrite) {
        return FALSE;
    }

    return InterlockedIncrement(&g_ControlSlotTraceCount) <= CTRL_SLOT_TRACE_LIMIT;
}

static
const char*
ControlSlotTypeName(
    _In_ UINT32 SlotType
)
{
    switch (SlotType) {
    case YumeDiskSlotTypeRead:
        return "read";
    case YumeDiskSlotTypeWrite:
        return "write";
    default:
        return "invalid";
    }
}

static
PYUMEDISK_MESSAGE
ControlGetMiniportMessage(
    _In_reads_bytes_(IoctlBufferSize) PUCHAR IoctlBuffer,
    _In_ ULONG IoctlBufferSize
)
{
    if (IoctlBuffer == NULL ||
        IoctlBufferSize < sizeof(SRB_IO_CONTROL) + (ULONG)YUMEDISK_MESSAGE_BASE_SIZE) {
        return NULL;
    }

    return (PYUMEDISK_MESSAGE)(((PSRB_IO_CONTROL)IoctlBuffer) + 1);
}

static
NTSTATUS
ControlMapSubmitSlotStatus(
    _In_ NTSTATUS Status,
    _In_reads_bytes_opt_(IoctlBufferSize) PUCHAR IoctlBuffer,
    _In_ ULONG IoctlBufferSize
)
{
    PYUMEDISK_MESSAGE message;

    if (Status != STATUS_IO_DEVICE_ERROR) {
        return Status;
    }

    message = ControlGetMiniportMessage(IoctlBuffer, IoctlBufferSize);
    if (message != NULL && message->Header.Command == YumeDiskCommandSubmitSlot) {
        return STATUS_CANCELLED;
    }

    return Status;
}

static
NTSTATUS
ControlCompleteAsyncSlotRequest(
    _Inout_ PCTRL_ASYNC_SLOT_REQUEST AsyncRequest,
    _In_ NTSTATUS IoStatus,
    _In_ ULONG_PTR Information
)
{
    NTSTATUS status;
    ULONG_PTR completionInformation;
    PSRB_IO_CONTROL srbIoControl;
    PYUMEDISK_MESSAGE message;
    ULONG transferLength;

    status = IoStatus;
    completionInformation = 0;
    if (NT_SUCCESS(status)) {
        if (Information < sizeof(SRB_IO_CONTROL) + YUMEDISK_MESSAGE_BASE_SIZE) {
            status = STATUS_DEVICE_PROTOCOL_ERROR;
        } else {
            srbIoControl = (PSRB_IO_CONTROL)AsyncRequest->IoctlBuffer;
            transferLength = (ULONG)min(
                (ULONG_PTR)(Information - sizeof(SRB_IO_CONTROL)),
                (ULONG_PTR)(AsyncRequest->IoctlBufferSize - sizeof(SRB_IO_CONTROL)));
            message = (PYUMEDISK_MESSAGE)(srbIoControl + 1);
            if (transferLength < (ULONG)YUMEDISK_MESSAGE_BASE_SIZE ||
                message->Header.Size > transferLength) {
                status = STATUS_DEVICE_PROTOCOL_ERROR;
            } else {
                status = (NTSTATUS)srbIoControl->ReturnCode;
            }
        }
    }

    status = ControlMapSubmitSlotStatus(status, AsyncRequest->IoctlBuffer, AsyncRequest->IoctlBufferSize);
    if (NT_SUCCESS(status)) {
        if (AsyncRequest->SlotType == YumeDiskSlotTypeRead) {
            if (AsyncRequest->DirectBuffer != NULL &&
                AsyncRequest->DirectBufferSize >= sizeof(YUMEDISK_READ_SLOT_EVENT)) {
                completionInformation = sizeof(YUMEDISK_READ_SLOT_EVENT);
            } else {
                status = STATUS_DEVICE_PROTOCOL_ERROR;
            }
        } else if (AsyncRequest->SlotType == YumeDiskSlotTypeWrite) {
            if (AsyncRequest->DirectBuffer != NULL &&
                AsyncRequest->DirectBufferSize >= YUMEDISK_WRITE_SLOT_HEADER_BASE_SIZE) {
                PYUMEDISK_WRITE_SLOT_HEADER writeHeader;

                writeHeader = (PYUMEDISK_WRITE_SLOT_HEADER)AsyncRequest->DirectBuffer;
                if ((size_t)writeHeader->DataLength <=
                    (AsyncRequest->DirectBufferSize - YUMEDISK_WRITE_SLOT_HEADER_BASE_SIZE)) {
                    completionInformation = YUMEDISK_WRITE_SLOT_HEADER_BASE_SIZE + writeHeader->DataLength;
                } else {
                    status = STATUS_DEVICE_PROTOCOL_ERROR;
                }
            } else {
                status = STATUS_DEVICE_PROTOCOL_ERROR;
            }
        } else {
            status = STATUS_INVALID_PARAMETER;
        }
    }

    if (ControlShouldTraceSlot(AsyncRequest->SlotType, status)) {
        DbgPrint(
            "YumeDiskKMDF slot complete request=%p async=%p irp=%p target=%lu slot=%I64u type=%s ioStatus=%08X final=%08X info=%Iu completeInfo=%Iu direct=%p directBytes=%Iu\n",
            AsyncRequest->Request,
            AsyncRequest,
            AsyncRequest->Irp,
            AsyncRequest->TargetId,
            (unsigned __int64)AsyncRequest->SlotId,
            ControlSlotTypeName(AsyncRequest->SlotType),
            (unsigned long)IoStatus,
            (unsigned long)status,
            Information,
            completionInformation,
            AsyncRequest->DirectBuffer,
            AsyncRequest->DirectBufferSize);
    }

    WdfRequestCompleteWithInformation(AsyncRequest->Request, status, completionInformation);
    ControlSessionReleaseSlot(AsyncRequest->SessionContext);
    return status;
}

_Function_class_(IO_COMPLETION_ROUTINE)
static
NTSTATUS
ControlSubmitSlotCompletionRoutine(
    _In_ PDEVICE_OBJECT DeviceObject,
    _In_ PIRP Irp,
    _In_ PVOID Context
)
{
    PCTRL_ASYNC_SLOT_REQUEST asyncRequest;

    UNREFERENCED_PARAMETER(DeviceObject);

    asyncRequest = (PCTRL_ASYNC_SLOT_REQUEST)Context;
    if (asyncRequest != NULL) {
        LONG ownerState;

        asyncRequest->CompletionStatus = Irp->IoStatus.Status;
        asyncRequest->CompletionInformation = Irp->IoStatus.Information;
        ownerState = InterlockedCompareExchange(&asyncRequest->CompletionState, 2, 0);
        if (ControlShouldTraceSlot(asyncRequest->SlotType, Irp->IoStatus.Status)) {
            DbgPrint(
                "YumeDiskKMDF slot completion routine async=%p irp=%p target=%lu slot=%I64u type=%s status=%08X info=%Iu owner=%ld\n",
                asyncRequest,
                Irp,
                asyncRequest->TargetId,
                (unsigned __int64)asyncRequest->SlotId,
                ControlSlotTypeName(asyncRequest->SlotType),
                (unsigned long)Irp->IoStatus.Status,
                Irp->IoStatus.Information,
                ownerState);
        }
        if (ownerState == 1) {
            (VOID)ControlCompleteAsyncSlotRequest(
                asyncRequest,
                Irp->IoStatus.Status,
                Irp->IoStatus.Information);
            ControlTransportRuntimeReleaseSlotRequest(asyncRequest);
        }
    }

    return STATUS_MORE_PROCESSING_REQUIRED;
}

VOID
ControlTransportDispatchSlotRequest(
    _Inout_ PCTRL_ASYNC_SLOT_REQUEST SlotRequest
)
{
    PCTRL_ASYNC_SLOT_REQUEST asyncRequest;
    NTSTATUS status;
    LONG ownerState;

    asyncRequest = SlotRequest;
    if (asyncRequest == NULL) {
        return;
    }

    status = IoCallDriver(asyncRequest->SessionContext->MiniportDeviceObject, asyncRequest->Irp);
    ownerState = InterlockedCompareExchange(&asyncRequest->CompletionState, 1, 0);
    if (ControlShouldTraceSlot(asyncRequest->SlotType, status)) {
        DbgPrint(
            "YumeDiskKMDF slot dispatch async=%p irp=%p target=%lu slot=%I64u type=%s ioCall=%08X owner=%ld irpStatus=%08X info=%Iu device=%p file=%p\n",
            asyncRequest,
            asyncRequest->Irp,
            asyncRequest->TargetId,
            (unsigned __int64)asyncRequest->SlotId,
            ControlSlotTypeName(asyncRequest->SlotType),
            (unsigned long)status,
            ownerState,
            (unsigned long)asyncRequest->Irp->IoStatus.Status,
            asyncRequest->Irp->IoStatus.Information,
            asyncRequest->SessionContext->MiniportDeviceObject,
            asyncRequest->SessionContext->MiniportFileObject);
    }
    if (ownerState == 2) {
        (VOID)ControlCompleteAsyncSlotRequest(
            asyncRequest,
            asyncRequest->CompletionStatus,
            asyncRequest->CompletionInformation);
        ControlTransportRuntimeReleaseSlotRequest(asyncRequest);
        return;
    }

    if (status != STATUS_PENDING && ownerState == 0) {
        NTSTATUS completionStatus;
        ULONG_PTR completionInformation;

        completionStatus = asyncRequest->Irp->IoStatus.Status;
        completionInformation = asyncRequest->Irp->IoStatus.Information;
        if (completionStatus == STATUS_NOT_SUPPORTED) {
            completionStatus = status;
        }

        (VOID)ControlCompleteAsyncSlotRequest(
            asyncRequest,
            completionStatus,
            completionInformation);
        ControlTransportRuntimeReleaseSlotRequest(asyncRequest);
    }
}

VOID
ControlTransportFailSlotRequest(
    _Inout_ PCTRL_ASYNC_SLOT_REQUEST SlotRequest,
    _In_ NTSTATUS Status
)
{
    if (SlotRequest == NULL) {
        return;
    }

    DbgPrint(
        "YumeDiskKMDF slot fail async=%p irp=%p target=%lu slot=%I64u type=%s status=%08X\n",
        SlotRequest,
        SlotRequest->Irp,
        SlotRequest->TargetId,
        (unsigned __int64)SlotRequest->SlotId,
        ControlSlotTypeName(SlotRequest->SlotType),
        (unsigned long)Status);

    (VOID)ControlCompleteAsyncSlotRequest(SlotRequest, Status, 0);
    ControlTransportRuntimeReleaseSlotRequest(SlotRequest);
}

static
NTSTATUS
ControlSendPreparedMiniportBuffer(
    _In_ HANDLE Handle,
    _In_ HANDLE EventHandle,
    _Inout_updates_bytes_(BufferCapacity) PUCHAR Buffer,
    _In_ ULONG InputLength,
    _In_ ULONG BufferCapacity,
    _Inout_updates_bytes_(IoctlBufferSize) PUCHAR IoctlBuffer,
    _In_ ULONG IoctlBufferSize,
    _Out_ ULONG* BytesReturned
)
{
    PSRB_IO_CONTROL srbIoControl;
    IO_STATUS_BLOCK ioStatus;
    NTSTATUS status;
    ULONG transferLength;
    PYUMEDISK_MESSAGE message;

    *BytesReturned = 0;

    if (Handle == NULL ||
        EventHandle == NULL ||
        Buffer == NULL ||
        IoctlBuffer == NULL ||
        BufferCapacity < (ULONG)YUMEDISK_MESSAGE_BASE_SIZE ||
        BufferCapacity > MAXULONG - sizeof(SRB_IO_CONTROL) ||
        IoctlBufferSize < sizeof(SRB_IO_CONTROL) + BufferCapacity ||
        InputLength < (ULONG)YUMEDISK_MESSAGE_BASE_SIZE ||
        InputLength > BufferCapacity) {
        return STATUS_INVALID_PARAMETER;
    }

    RtlZeroMemory(IoctlBuffer, IoctlBufferSize);
    srbIoControl = (PSRB_IO_CONTROL)IoctlBuffer;
    srbIoControl->HeaderLength = sizeof(SRB_IO_CONTROL);
    srbIoControl->Timeout = YUMEDISK_MINIPORT_TIMEOUT_SEC;
    srbIoControl->ControlCode = YUMEDISK_MINIPORT_CONTROL_CODE;
    srbIoControl->Length = BufferCapacity;
    RtlCopyMemory(srbIoControl->Signature, YUMEDISK_MINIPORT_SIGNATURE, sizeof(srbIoControl->Signature));
    RtlCopyMemory(srbIoControl + 1, Buffer, InputLength);

    status = ZwDeviceIoControlFile(
        Handle,
        EventHandle,
        NULL,
        NULL,
        &ioStatus,
        IOCTL_SCSI_MINIPORT,
        IoctlBuffer,
        IoctlBufferSize,
        IoctlBuffer,
        IoctlBufferSize);
    if (status == STATUS_PENDING) {
        status = ZwWaitForSingleObject(EventHandle, FALSE, NULL);
        if (NT_SUCCESS(status)) {
            status = ioStatus.Status;
        }
    } else if (NT_SUCCESS(status)) {
        status = ioStatus.Status;
    }

    if (NT_SUCCESS(status)) {
        if (ioStatus.Information < sizeof(SRB_IO_CONTROL) + YUMEDISK_MESSAGE_BASE_SIZE) {
            status = STATUS_DEVICE_PROTOCOL_ERROR;
        } else {
            transferLength = (ULONG)min(
                (ULONG_PTR)(ioStatus.Information - sizeof(SRB_IO_CONTROL)),
                (ULONG_PTR)BufferCapacity);
            RtlCopyMemory(Buffer, srbIoControl + 1, transferLength);
            *BytesReturned = transferLength;

            message = (PYUMEDISK_MESSAGE)Buffer;
            if (transferLength < (ULONG)YUMEDISK_MESSAGE_BASE_SIZE ||
                message->Header.Size > transferLength) {
                status = STATUS_DEVICE_PROTOCOL_ERROR;
            } else {
                status = (NTSTATUS)srbIoControl->ReturnCode;
            }
        }
    }

    return status;
}

static
NTSTATUS
ControlSendMiniportBuffer(
    _In_ HANDLE Handle,
    _Inout_updates_bytes_(BufferCapacity) PUCHAR Buffer,
    _In_ ULONG InputLength,
    _In_ ULONG BufferCapacity,
    _Out_ ULONG* BytesReturned
)
{
    ULONG ioctlBufferSize;
    PUCHAR ioctlBuffer;
    OBJECT_ATTRIBUTES eventAttributes;
    HANDLE eventHandle;
    NTSTATUS status;

    *BytesReturned = 0;

    if (Handle == NULL ||
        Buffer == NULL ||
        BufferCapacity < (ULONG)YUMEDISK_MESSAGE_BASE_SIZE ||
        BufferCapacity > MAXULONG - sizeof(SRB_IO_CONTROL) ||
        InputLength < (ULONG)YUMEDISK_MESSAGE_BASE_SIZE ||
        InputLength > BufferCapacity) {
        return STATUS_INVALID_PARAMETER;
    }

    ioctlBufferSize = sizeof(SRB_IO_CONTROL) + BufferCapacity;
    ioctlBuffer = (PUCHAR)ControlAlloc(ioctlBufferSize);
    if (ioctlBuffer == NULL) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    InitializeObjectAttributes(
        &eventAttributes,
        NULL,
        OBJ_KERNEL_HANDLE,
        NULL,
        NULL);
    status = ZwCreateEvent(
        &eventHandle,
        SYNCHRONIZE | EVENT_MODIFY_STATE,
        &eventAttributes,
        NotificationEvent,
        FALSE);
    if (!NT_SUCCESS(status)) {
        ControlFree(ioctlBuffer);
        return status;
    }

    status = ControlSendPreparedMiniportBuffer(
        Handle,
        eventHandle,
        Buffer,
        InputLength,
        BufferCapacity,
        ioctlBuffer,
        ioctlBufferSize,
        BytesReturned);

    ZwClose(eventHandle);
    ControlFree(ioctlBuffer);
    return status;
}

static
NTSTATUS
ControlProbeMiniportHandle(
    _In_ HANDLE Handle
)
{
    UCHAR buffer[YUMEDISK_MESSAGE_BASE_SIZE + sizeof(YUMEDISK_QUERY_INFO)];
    PYUMEDISK_MESSAGE message;
    PYUMEDISK_QUERY_INFO info;
    ULONG bytesReturned;
    NTSTATUS status;

    RtlZeroMemory(buffer, sizeof(buffer));
    message = (PYUMEDISK_MESSAGE)buffer;
    message->Header.Size = sizeof(buffer);
    message->Header.Version = YUMEDISK_PROTOCOL_VERSION;
    message->Header.Command = YumeDiskCommandQueryInfo;

    status = ControlSendMiniportBuffer(
        Handle,
        buffer,
        YUMEDISK_MESSAGE_BASE_SIZE,
        sizeof(buffer),
        &bytesReturned);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    if (bytesReturned < YUMEDISK_MESSAGE_BASE_SIZE + sizeof(YUMEDISK_QUERY_INFO)) {
        return STATUS_DEVICE_PROTOCOL_ERROR;
    }

    info = (PYUMEDISK_QUERY_INFO)message->Payload;
    if (RtlCompareMemory(
            info->AdapterSignature,
            YUMEDISK_MINIPORT_SIGNATURE,
            sizeof(info->AdapterSignature)) != sizeof(info->AdapterSignature)) {
        return STATUS_NOT_FOUND;
    }

    return STATUS_SUCCESS;
}

NTSTATUS
ControlOpenMiniportHandle(
    _Out_ HANDLE* Handle,
    _Out_opt_ PFILE_OBJECT* FileObject,
    _Out_opt_ PDEVICE_OBJECT* DeviceObject
)
{
    PWSTR interfaces;
    PWSTR current;
    NTSTATUS status;

    *Handle = NULL;
    if (FileObject != NULL) {
        *FileObject = NULL;
    }
    if (DeviceObject != NULL) {
        *DeviceObject = NULL;
    }

    interfaces = NULL;
    status = IoGetDeviceInterfaces((LPGUID)&ControlStoragePortGuid, NULL, 0, &interfaces);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    status = STATUS_NO_SUCH_DEVICE;
    current = interfaces;
    while (*current != UNICODE_NULL) {
        UNICODE_STRING deviceName;
        OBJECT_ATTRIBUTES attributes;
        IO_STATUS_BLOCK ioStatus;
        HANDLE handle;
        PFILE_OBJECT fileObject;
        PDEVICE_OBJECT deviceObject;
        NTSTATUS openStatus;

        RtlInitUnicodeString(&deviceName, current);
        InitializeObjectAttributes(
            &attributes,
            &deviceName,
            OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE,
            NULL,
            NULL);

        openStatus = ZwCreateFile(
            &handle,
            GENERIC_READ | GENERIC_WRITE | SYNCHRONIZE,
            &attributes,
            &ioStatus,
            NULL,
            FILE_ATTRIBUTE_NORMAL,
            FILE_SHARE_READ | FILE_SHARE_WRITE,
            FILE_OPEN,
            FILE_NON_DIRECTORY_FILE,
            NULL,
            0);
        if (NT_SUCCESS(openStatus)) {
            openStatus = ControlProbeMiniportHandle(handle);
            if (NT_SUCCESS(openStatus)) {
                fileObject = NULL;
                deviceObject = NULL;
                openStatus = ObReferenceObjectByHandle(
                    handle,
                    0,
                    NULL,
                    KernelMode,
                    (PVOID*)&fileObject,
                    NULL);
                if (NT_SUCCESS(openStatus)) {
                    deviceObject = IoGetRelatedDeviceObject(fileObject);
                    if (deviceObject == NULL) {
                        ObDereferenceObject(fileObject);
                        fileObject = NULL;
                        openStatus = STATUS_NO_SUCH_DEVICE;
                    }
                }
            } else {
                fileObject = NULL;
                deviceObject = NULL;
            }

            if (NT_SUCCESS(openStatus)) {
                *Handle = handle;
                if (FileObject != NULL) {
                    *FileObject = fileObject;
                }
                if (DeviceObject != NULL) {
                    *DeviceObject = deviceObject;
                }
                status = STATUS_SUCCESS;
                break;
            }

            ZwClose(handle);
        }

        current += wcslen(current) + 1;
    }

    ExFreePool(interfaces);
    return status;
}

NTSTATUS
ControlProxyCommand(
    _In_ PCTRL_FILE_CONTEXT Context,
    _Inout_updates_bytes_(BufferCapacity) PUCHAR Buffer,
    _In_ ULONG InputLength,
    _In_ ULONG BufferCapacity,
    _Out_ ULONG* BytesReturned
)
{
    PCTRL_SYNC_COMMAND_BUFFER commandBuffer;
    ULONG ioctlBufferSize;
    NTSTATUS status;

    if (Context == NULL || Context->MiniportHandle == NULL) {
        return STATUS_DEVICE_NOT_READY;
    }

    if (BufferCapacity > MAXULONG - sizeof(SRB_IO_CONTROL)) {
        return STATUS_INVALID_PARAMETER;
    }

    ioctlBufferSize = sizeof(SRB_IO_CONTROL) + BufferCapacity;
    commandBuffer = NULL;
    status = ControlTransportRuntimeAcquireSyncCommandBuffer(Context, ioctlBufferSize, &commandBuffer);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    status = ControlSendPreparedMiniportBuffer(
        Context->MiniportHandle,
        commandBuffer->EventHandle,
        Buffer,
        InputLength,
        BufferCapacity,
        commandBuffer->IoctlBuffer,
        commandBuffer->IoctlBufferSize,
        BytesReturned);

    ControlTransportRuntimeReleaseSyncCommandBuffer(commandBuffer);
    return status;
}

NTSTATUS
ControlProxySubmitSlotAsync(
    _In_ PCTRL_FILE_CONTEXT Context,
    _In_ WDFREQUEST Request,
    _In_ UINT64 SessionId,
    _In_ UINT64 SlotId,
    _In_ UINT32 TargetId,
    _In_ UINT32 SlotType,
    _In_ PUCHAR DirectBuffer,
    _In_ size_t DirectBufferSize
)
{
    NTSTATUS status;
    PCTRL_ASYNC_SLOT_REQUEST asyncRequest;
    ULONG bufferSize;
    ULONG ioctlBufferSize;
    PIRP irp;
    PIO_STACK_LOCATION stack;
    PSRB_IO_CONTROL srbIoControl;
    PYUMEDISK_MESSAGE message;
    PYUMEDISK_SUBMIT_SLOT submitSlot;

    if (Context == NULL ||
        Context->MiniportHandle == NULL ||
        Context->MiniportFileObject == NULL ||
        Context->MiniportDeviceObject == NULL ||
        Request == NULL ||
        DirectBuffer == NULL ||
        DirectBufferSize > MAXULONG ||
        SlotId == 0 ||
        (SlotType != YumeDiskSlotTypeRead && SlotType != YumeDiskSlotTypeWrite)) {
        return STATUS_INVALID_PARAMETER;
    }

    bufferSize = YUMEDISK_MESSAGE_BASE_SIZE + YUMEDISK_SUBMIT_SLOT_SIZE();
    ioctlBufferSize = sizeof(SRB_IO_CONTROL) + bufferSize;

    status = ControlTransportRuntimeAcquireSlotRequest(
        Context,
        Context->MiniportDeviceObject->StackSize,
        ioctlBufferSize,
        &asyncRequest);
    if (!NT_SUCCESS(status)) {
        DbgPrint(
            "YumeDiskKMDF slot acquire failed target=%lu slot=%I64u type=%s status=%08X stack=%d bytes=%lu\n",
            TargetId,
            (unsigned __int64)SlotId,
            ControlSlotTypeName(SlotType),
            (unsigned long)status,
            Context->MiniportDeviceObject->StackSize,
            ioctlBufferSize);
        return status;
    }

    asyncRequest->SessionContext = Context;
    asyncRequest->Request = Request;
    asyncRequest->SlotId = SlotId;
    asyncRequest->TargetId = TargetId;
    asyncRequest->SlotType = SlotType;
    asyncRequest->DirectBuffer = DirectBuffer;
    asyncRequest->DirectBufferSize = DirectBufferSize;
    asyncRequest->CompletionState = 0;
    asyncRequest->CompletionStatus = STATUS_UNSUCCESSFUL;
    asyncRequest->CompletionInformation = 0;

    RtlZeroMemory(asyncRequest->IoctlBuffer, ioctlBufferSize);

    srbIoControl = (PSRB_IO_CONTROL)asyncRequest->IoctlBuffer;
    srbIoControl->HeaderLength = sizeof(SRB_IO_CONTROL);
    srbIoControl->Timeout = YUMEDISK_MINIPORT_TIMEOUT_SEC;
    srbIoControl->ControlCode = YUMEDISK_MINIPORT_CONTROL_CODE;
    srbIoControl->Length = bufferSize;
    RtlCopyMemory(srbIoControl->Signature, YUMEDISK_MINIPORT_SIGNATURE, sizeof(srbIoControl->Signature));

    message = (PYUMEDISK_MESSAGE)(srbIoControl + 1);
    message->Header.Size = bufferSize;
    message->Header.Version = YUMEDISK_PROTOCOL_VERSION;
    message->Header.Command = YumeDiskCommandSubmitSlot;
    message->Header.SessionId = SessionId;
    message->Header.TargetId = TargetId;
    message->Header.PayloadLength = YUMEDISK_SUBMIT_SLOT_SIZE();

    submitSlot = (PYUMEDISK_SUBMIT_SLOT)message->Payload;
    submitSlot->Slot.SessionId = SessionId;
    submitSlot->Slot.SlotId = SlotId;
    submitSlot->Slot.SlotType = SlotType;
    submitSlot->Slot.TargetId = TargetId;
    submitSlot->Slot.KernelVa = (UINT64)(ULONG_PTR)DirectBuffer;
    submitSlot->Slot.Capacity = (UINT32)DirectBufferSize;
    submitSlot->Slot.Flags = YumeDiskSlotFlagNone;

    irp = IoAllocateIrp(Context->MiniportDeviceObject->StackSize, FALSE);
    if (irp == NULL) {
        ControlTransportRuntimeReleaseSlotRequest(asyncRequest);
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    asyncRequest->Irp = irp;
    asyncRequest->IrpHasBeenSubmitted = TRUE;

    irp->RequestorMode = KernelMode;
    irp->AssociatedIrp.SystemBuffer = asyncRequest->IoctlBuffer;
    irp->UserBuffer = asyncRequest->IoctlBuffer;
    irp->Flags = IRP_BUFFERED_IO;
    irp->IoStatus.Status = STATUS_NOT_SUPPORTED;
    irp->IoStatus.Information = 0;

    stack = IoGetNextIrpStackLocation(irp);
    stack->MajorFunction = IRP_MJ_DEVICE_CONTROL;
    stack->FileObject = Context->MiniportFileObject;
    stack->Parameters.DeviceIoControl.IoControlCode = IOCTL_SCSI_MINIPORT;
    stack->Parameters.DeviceIoControl.InputBufferLength = ioctlBufferSize;
    stack->Parameters.DeviceIoControl.OutputBufferLength = ioctlBufferSize;
    stack->Parameters.DeviceIoControl.Type3InputBuffer = NULL;

    IoSetCompletionRoutine(
        irp,
        ControlSubmitSlotCompletionRoutine,
        asyncRequest,
        TRUE,
        TRUE,
        TRUE);

    status = ControlTransportRuntimeSubmitSlotRequest(asyncRequest);
    if (!NT_SUCCESS(status)) {
        DbgPrint(
            "YumeDiskKMDF slot enqueue failed async=%p irp=%p target=%lu slot=%I64u type=%s status=%08X\n",
            asyncRequest,
            asyncRequest->Irp,
            TargetId,
            (unsigned __int64)SlotId,
            ControlSlotTypeName(SlotType),
            (unsigned long)status);
        ControlTransportRuntimeReleaseSlotRequest(asyncRequest);
        return status;
    }

    if (ControlShouldTraceSlot(SlotType, STATUS_SUCCESS)) {
        DbgPrint(
            "YumeDiskKMDF slot enqueue async=%p irp=%p request=%p target=%lu slot=%I64u type=%s direct=%p directBytes=%Iu\n",
            asyncRequest,
            asyncRequest->Irp,
            Request,
            TargetId,
            (unsigned __int64)SlotId,
            ControlSlotTypeName(SlotType),
            DirectBuffer,
            DirectBufferSize);
    }

    return STATUS_PENDING;
}

VOID
ControlCloseMiniportHandle(
    _Inout_ PCTRL_FILE_CONTEXT Context
)
{
    HANDLE handle;
    PFILE_OBJECT fileObject;

    if (Context == NULL) {
        return;
    }

    handle = Context->MiniportHandle;
    fileObject = Context->MiniportFileObject;
    Context->MiniportHandle = NULL;
    Context->MiniportFileObject = NULL;
    Context->MiniportDeviceObject = NULL;

    if (handle != NULL) {
        ZwClose(handle);
    }
    if (fileObject != NULL) {
        ObDereferenceObject(fileObject);
    }
}

VOID
ControlSendSessionCleanup(
    _In_ PCTRL_FILE_CONTEXT Context
)
{
    UCHAR buffer[YUMEDISK_MESSAGE_BASE_SIZE];
    PYUMEDISK_MESSAGE message;
    ULONG bytesReturned;

    if (Context == NULL || Context->MiniportHandle == NULL || Context->SessionId == 0) {
        return;
    }

    RtlZeroMemory(buffer, sizeof(buffer));
    message = (PYUMEDISK_MESSAGE)buffer;
    message->Header.Size = sizeof(buffer);
    message->Header.Version = YUMEDISK_PROTOCOL_VERSION;
    message->Header.Command = YumeDiskCommandRemoveAllDisks;
    message->Header.SessionId = Context->SessionId;
    message->Header.Flags = YUMEDISK_SESSION_CLOSE_FLAG;

    (VOID)ControlSendMiniportBuffer(Context->MiniportHandle, buffer, sizeof(buffer), sizeof(buffer), &bytesReturned);
}
