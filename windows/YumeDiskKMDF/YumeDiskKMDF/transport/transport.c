#include "transport.h"

#include <ntddscsi.h>

#include "..\core\memory.h"
#include "..\session\session.h"

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

typedef struct _CTRL_ASYNC_SLOT_REQUEST {
    PCTRL_FILE_CONTEXT SessionContext;
    WDFREQUEST Request;
    PUCHAR IoctlBuffer;
    ULONG IoctlBufferSize;
    IO_STATUS_BLOCK IoStatusBlock;
    volatile LONG DispatchState;
    volatile LONG CompletedInline;
    NTSTATUS CompletionStatus;
    ULONG_PTR CompletionInformation;
} CTRL_ASYNC_SLOT_REQUEST, *PCTRL_ASYNC_SLOT_REQUEST;

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
VOID
ControlFreeAsyncSlotRequest(
    _Inout_opt_ PCTRL_ASYNC_SLOT_REQUEST AsyncRequest
)
{
    if (AsyncRequest == NULL) {
        return;
    }

    ControlFree(AsyncRequest->IoctlBuffer);
    ControlFree(AsyncRequest);
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
    PSRB_IO_CONTROL srbIoControl;
    PYUMEDISK_MESSAGE message;
    ULONG transferLength;

    status = IoStatus;
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
    WdfRequestCompleteWithInformation(AsyncRequest->Request, status, 0);
    ControlSessionUnregisterPendingSlot(AsyncRequest->SessionContext);
    ControlSessionRelease(AsyncRequest->SessionContext);
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
        asyncRequest->CompletionStatus = Irp->IoStatus.Status;
        asyncRequest->CompletionInformation = Irp->IoStatus.Information;
        if (InterlockedCompareExchange(&asyncRequest->DispatchState, 2, 1) == 1) {
            (VOID)ControlCompleteAsyncSlotRequest(
                asyncRequest,
                asyncRequest->CompletionStatus,
                asyncRequest->CompletionInformation);
            ControlFreeAsyncSlotRequest(asyncRequest);
        } else {
            InterlockedExchange(&asyncRequest->CompletedInline, 1);
        }
    }

    return STATUS_CONTINUE_COMPLETION;
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
    PSRB_IO_CONTROL srbIoControl;
    IO_STATUS_BLOCK ioStatus;
    OBJECT_ATTRIBUTES eventAttributes;
    HANDLE eventHandle;
    NTSTATUS status;
    ULONG transferLength;
    PYUMEDISK_MESSAGE message;

    *BytesReturned = 0;

    if (Handle == NULL ||
        Buffer == NULL ||
        BufferCapacity < (ULONG)YUMEDISK_MESSAGE_BASE_SIZE ||
        InputLength < (ULONG)YUMEDISK_MESSAGE_BASE_SIZE ||
        InputLength > BufferCapacity) {
        return STATUS_INVALID_PARAMETER;
    }

    ioctlBufferSize = sizeof(SRB_IO_CONTROL) + BufferCapacity;
    ioctlBuffer = (PUCHAR)ControlAlloc(ioctlBufferSize);
    if (ioctlBuffer == NULL) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    RtlZeroMemory(ioctlBuffer, ioctlBufferSize);
    srbIoControl = (PSRB_IO_CONTROL)ioctlBuffer;
    srbIoControl->HeaderLength = sizeof(SRB_IO_CONTROL);
    srbIoControl->Timeout = YUMEDISK_MINIPORT_TIMEOUT_SEC;
    srbIoControl->ControlCode = YUMEDISK_MINIPORT_CONTROL_CODE;
    srbIoControl->Length = BufferCapacity;
    RtlCopyMemory(srbIoControl->Signature, YUMEDISK_MINIPORT_SIGNATURE, sizeof(srbIoControl->Signature));
    RtlCopyMemory(srbIoControl + 1, Buffer, InputLength);

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

    status = ZwDeviceIoControlFile(
        Handle,
        eventHandle,
        NULL,
        NULL,
        &ioStatus,
        IOCTL_SCSI_MINIPORT,
        ioctlBuffer,
        ioctlBufferSize,
        ioctlBuffer,
        ioctlBufferSize);
    if (status == STATUS_PENDING) {
        status = ZwWaitForSingleObject(eventHandle, FALSE, NULL);
        if (NT_SUCCESS(status)) {
            status = ioStatus.Status;
        }
    } else if (NT_SUCCESS(status)) {
        status = ioStatus.Status;
    }

    ZwClose(eventHandle);

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
    if (Context == NULL || Context->MiniportHandle == NULL) {
        return STATUS_DEVICE_NOT_READY;
    }

    return ControlSendMiniportBuffer(Context->MiniportHandle, Buffer, InputLength, BufferCapacity, BytesReturned);
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

    status = ControlSessionRegisterPendingSlot(Context);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    asyncRequest = (PCTRL_ASYNC_SLOT_REQUEST)ControlAlloc(sizeof(*asyncRequest));
    if (asyncRequest == NULL) {
        ControlSessionUnregisterPendingSlot(Context);
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    RtlZeroMemory(asyncRequest, sizeof(*asyncRequest));
    asyncRequest->SessionContext = Context;
    asyncRequest->Request = Request;
    asyncRequest->DispatchState = 0;
    asyncRequest->CompletedInline = 0;
    asyncRequest->CompletionStatus = STATUS_UNSUCCESSFUL;
    asyncRequest->CompletionInformation = 0;

    bufferSize = YUMEDISK_MESSAGE_BASE_SIZE + YUMEDISK_SUBMIT_SLOT_SIZE();
    ioctlBufferSize = sizeof(SRB_IO_CONTROL) + bufferSize;
    asyncRequest->IoctlBuffer = (PUCHAR)ControlAlloc(ioctlBufferSize);
    if (asyncRequest->IoctlBuffer == NULL) {
        ControlSessionUnregisterPendingSlot(Context);
        ControlFreeAsyncSlotRequest(asyncRequest);
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    asyncRequest->IoctlBufferSize = ioctlBufferSize;
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

    irp = IoBuildDeviceIoControlRequest(
        IOCTL_SCSI_MINIPORT,
        Context->MiniportDeviceObject,
        asyncRequest->IoctlBuffer,
        ioctlBufferSize,
        asyncRequest->IoctlBuffer,
        ioctlBufferSize,
        FALSE,
        NULL,
        &asyncRequest->IoStatusBlock);
    if (irp == NULL) {
        ControlSessionUnregisterPendingSlot(Context);
        ControlFreeAsyncSlotRequest(asyncRequest);
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    irp->Tail.Overlay.OriginalFileObject = Context->MiniportFileObject;

    stack = IoGetNextIrpStackLocation(irp);
    stack->FileObject = Context->MiniportFileObject;

    IoSetCompletionRoutine(
        irp,
        ControlSubmitSlotCompletionRoutine,
        asyncRequest,
        TRUE,
        TRUE,
        TRUE);

    status = IoCallDriver(Context->MiniportDeviceObject, irp);
    if (status == STATUS_PENDING) {
        (VOID)InterlockedCompareExchange(&asyncRequest->DispatchState, 1, 0);
        return STATUS_PENDING;
    }

    if (InterlockedExchange(&asyncRequest->CompletedInline, 0) != 0) {
        (VOID)ControlCompleteAsyncSlotRequest(
            asyncRequest,
            asyncRequest->CompletionStatus,
            asyncRequest->CompletionInformation);
        ControlFreeAsyncSlotRequest(asyncRequest);
        return STATUS_SUCCESS;
    }

    ControlSessionUnregisterPendingSlot(Context);
    ControlFreeAsyncSlotRequest(asyncRequest);
    return status;
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

    (VOID)ControlProxyCommand(Context, buffer, sizeof(buffer), sizeof(buffer), &bytesReturned);
}
