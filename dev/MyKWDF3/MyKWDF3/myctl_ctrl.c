#include "myctl_ctrl.h"

#include <ntddscsi.h>

#include "myctl_defs.h"
#include "myctl_utils.h"

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(CTRL_DEVICE_CONTEXT, MyCtlGetContext);

static const GUID MyCtlStoragePortGuid = {
    0x2accfe60, 0xc130, 0x11d2, { 0xb0, 0x82, 0x00, 0xa0, 0xc9, 0x1e, 0xfb, 0x8b }
};

static
UINT64
MyCtlGenerateSessionId(
    VOID
)
{
    LARGE_INTEGER tick;

    KeQuerySystemTimePrecise(&tick);
    return ((UINT64)tick.QuadPart) ^ (UINT64)(ULONG_PTR)PsGetCurrentProcessId();
}

static
NTSTATUS
MyCtlSendMiniportBuffer(
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
    NTSTATUS status;
    ULONG transferLength;
    PYUMEDISK_MESSAGE message;

    *BytesReturned = 0;

    if (Buffer == NULL ||
        BufferCapacity < (ULONG)YUMEDISK_MESSAGE_BASE_SIZE ||
        InputLength < (ULONG)YUMEDISK_MESSAGE_BASE_SIZE ||
        InputLength > BufferCapacity) {
        return STATUS_INVALID_PARAMETER;
    }

    ioctlBufferSize = sizeof(SRB_IO_CONTROL) + BufferCapacity;
    ioctlBuffer = (PUCHAR)MyCtlAlloc(ioctlBufferSize);
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

    status = ZwDeviceIoControlFile(
        Handle,
        NULL,
        NULL,
        NULL,
        &ioStatus,
        IOCTL_SCSI_MINIPORT,
        ioctlBuffer,
        ioctlBufferSize,
        ioctlBuffer,
        ioctlBufferSize
    );

    if (NT_SUCCESS(status)) {
        status = ioStatus.Status;
    }

    if (NT_SUCCESS(status)) {
        if (ioStatus.Information < sizeof(SRB_IO_CONTROL) + YUMEDISK_MESSAGE_BASE_SIZE) {
            status = STATUS_DEVICE_PROTOCOL_ERROR;
        } else {
            transferLength = (ULONG)min((ULONG_PTR)(ioStatus.Information - sizeof(SRB_IO_CONTROL)), (ULONG_PTR)BufferCapacity);
            RtlCopyMemory(Buffer, srbIoControl + 1, transferLength);
            *BytesReturned = transferLength;

            message = (PYUMEDISK_MESSAGE)Buffer;
            if (transferLength < (ULONG)YUMEDISK_MESSAGE_BASE_SIZE || message->Header.Size > transferLength) {
                status = STATUS_DEVICE_PROTOCOL_ERROR;
            }
        }
    }

    MyCtlFree(ioctlBuffer);
    return status;
}

static
NTSTATUS
MyCtlProbeMiniportHandle(
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

    status = MyCtlSendMiniportBuffer(Handle, buffer, YUMEDISK_MESSAGE_BASE_SIZE, sizeof(buffer), &bytesReturned);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    if (bytesReturned < YUMEDISK_MESSAGE_BASE_SIZE + sizeof(YUMEDISK_QUERY_INFO)) {
        return STATUS_DEVICE_PROTOCOL_ERROR;
    }

    info = (PYUMEDISK_QUERY_INFO)message->Payload;
    if (RtlCompareMemory(info->AdapterSignature, YUMEDISK_MINIPORT_SIGNATURE, sizeof(info->AdapterSignature)) != sizeof(info->AdapterSignature)) {
        return STATUS_NOT_FOUND;
    }

    return STATUS_SUCCESS;
}

static
NTSTATUS
MyCtlOpenMiniportHandle(
    _Out_ HANDLE* Handle
)
{
    PWSTR interfaces;
    PWSTR current;
    NTSTATUS status;

    *Handle = NULL;
    interfaces = NULL;

    status = IoGetDeviceInterfaces(
        (LPGUID)&MyCtlStoragePortGuid,
        NULL,
        0,
        &interfaces
    );
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
        NTSTATUS openStatus;

        RtlInitUnicodeString(&deviceName, current);
        InitializeObjectAttributes(&attributes, &deviceName, OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE, NULL, NULL);

        openStatus = ZwCreateFile(
            &handle,
            GENERIC_READ | GENERIC_WRITE | SYNCHRONIZE,
            &attributes,
            &ioStatus,
            NULL,
            FILE_ATTRIBUTE_NORMAL,
            FILE_SHARE_READ | FILE_SHARE_WRITE,
            FILE_OPEN,
            FILE_NON_DIRECTORY_FILE | FILE_SYNCHRONOUS_IO_NONALERT,
            NULL,
            0
        );

        if (NT_SUCCESS(openStatus)) {
            openStatus = MyCtlProbeMiniportHandle(handle);
            if (NT_SUCCESS(openStatus)) {
                *Handle = handle;
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

static
NTSTATUS
MyCtlProxyCommand(
    _Inout_updates_bytes_(BufferCapacity) PUCHAR Buffer,
    _In_ ULONG InputLength,
    _In_ ULONG BufferCapacity,
    _Out_ ULONG* BytesReturned
)
{
    HANDLE handle;
    NTSTATUS status;

    handle = NULL;
    status = MyCtlOpenMiniportHandle(&handle);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    status = MyCtlSendMiniportBuffer(handle, Buffer, InputLength, BufferCapacity, BytesReturned);
    ZwClose(handle);
    return status;
}

static
VOID
MyCtlSendSessionCleanup(
    _In_ UINT64 SessionId
)
{
    UCHAR buffer[YUMEDISK_MESSAGE_BASE_SIZE];
    PYUMEDISK_MESSAGE message;
    ULONG bytesReturned;

    RtlZeroMemory(buffer, sizeof(buffer));
    message = (PYUMEDISK_MESSAGE)buffer;
    message->Header.Size = sizeof(buffer);
    message->Header.Version = YUMEDISK_PROTOCOL_VERSION;
    message->Header.Command = YumeDiskCommandRemoveAllDisks;
    message->Header.SessionId = SessionId;
    message->Header.Flags = YUMEDISK_SESSION_CLOSE_FLAG;

    (VOID)MyCtlProxyCommand(buffer, sizeof(buffer), sizeof(buffer), &bytesReturned);
}

static
VOID
MyCtlFileCreate(
    _In_ WDFDEVICE Device,
    _In_ WDFREQUEST Request,
    _In_ WDFFILEOBJECT FileObject
)
{
    PCTRL_DEVICE_CONTEXT context;
    NTSTATUS status;

    UNREFERENCED_PARAMETER(FileObject);

    context = MyCtlGetContext(Device);
    status = STATUS_SUCCESS;

    WdfSpinLockAcquire(context->OpenLock);
    if (context->OpenCount != 0) {
        status = STATUS_SHARING_VIOLATION;
    } else {
        context->OpenCount = 1;
        context->OpenFileObject = FileObject;
        context->SessionId = MyCtlGenerateSessionId();
    }
    WdfSpinLockRelease(context->OpenLock);

    WdfRequestComplete(Request, status);
}

static
VOID
MyCtlFileCleanup(
    _In_ WDFFILEOBJECT FileObject
)
{
    WDFDEVICE device;
    PCTRL_DEVICE_CONTEXT context;
    UINT64 sessionId;

    device = WdfFileObjectGetDevice(FileObject);
    context = MyCtlGetContext(device);
    sessionId = 0;

    WdfSpinLockAcquire(context->OpenLock);
    if (context->OpenCount != 0 && context->OpenFileObject == FileObject) {
        sessionId = context->SessionId;
        context->OpenCount = 0;
        context->OpenFileObject = NULL;
        context->SessionId = 0;
    }
    WdfSpinLockRelease(context->OpenLock);

    if (sessionId != 0) {
        MyCtlSendSessionCleanup(sessionId);
    }
}

static
VOID
MyCtlFileClose(
    _In_ WDFFILEOBJECT FileObject
)
{
    UNREFERENCED_PARAMETER(FileObject);
}

static
VOID
MyCtlDeviceControl(
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
    NTSTATUS status;
    ULONG bytesReturned;
    ULONG bufferCapacity;
    UINT64 sessionId;
    PYUMEDISK_MESSAGE message;

    device = WdfIoQueueGetDevice(Queue);
    context = MyCtlGetContext(device);
    inputBuffer = NULL;
    outputBuffer = NULL;
    bytesReturned = 0;
    sessionId = 0;

    if (IoControlCode != IOCTL_YUMEDISK_APP_COMMAND) {
        WdfRequestComplete(Request, STATUS_INVALID_DEVICE_REQUEST);
        return;
    }

    if (InputBufferLength < YUMEDISK_MESSAGE_BASE_SIZE || OutputBufferLength < YUMEDISK_MESSAGE_BASE_SIZE) {
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

    bufferCapacity = (ULONG)((inputSize > outputSize ? inputSize : outputSize) > MAXULONG ?
        MAXULONG :
        (inputSize > outputSize ? inputSize : outputSize));
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

    WdfSpinLockAcquire(context->OpenLock);
    if (context->OpenCount == 1 && context->OpenFileObject != NULL) {
        sessionId = context->SessionId;
    }
    WdfSpinLockRelease(context->OpenLock);

    if (sessionId == 0) {
        WdfRequestComplete(Request, STATUS_DEVICE_NOT_READY);
        return;
    }

    message->Header.SessionId = sessionId;
    status = MyCtlProxyCommand(outputBuffer, (ULONG)inputSize, bufferCapacity, &bytesReturned);
    if (bytesReturned == 0) {
        bytesReturned = YUMEDISK_MESSAGE_BASE_SIZE;
    }

    if (bytesReturned >= (ULONG)YUMEDISK_MESSAGE_BASE_SIZE) {
        message->Header.SessionId = sessionId;
    }

    WdfRequestCompleteWithInformation(Request, status, bytesReturned);
}

NTSTATUS
MyCtlAdd(
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

    WdfDeviceInitSetExclusive(DeviceInit, TRUE);
    WdfDeviceInitSetIoType(DeviceInit, WdfDeviceIoBuffered);

    WDF_FILEOBJECT_CONFIG_INIT(&fileConfig, MyCtlFileCreate, MyCtlFileClose, MyCtlFileCleanup);
    WdfDeviceInitSetFileObjectConfig(DeviceInit, &fileConfig, WDF_NO_OBJECT_ATTRIBUTES);

    WDF_PNPPOWER_EVENT_CALLBACKS_INIT(&pnpCallbacks);
    WdfDeviceInitSetPnpPowerEventCallbacks(DeviceInit, &pnpCallbacks);

    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attributes, CTRL_DEVICE_CONTEXT);
    attributes.SynchronizationScope = WdfSynchronizationScopeNone;

    status = WdfDeviceCreate(&DeviceInit, &attributes, &device);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    context = MyCtlGetContext(device);
    context->OpenCount = 0;
    context->OpenFileObject = NULL;
    context->SessionId = 0;

    WDF_OBJECT_ATTRIBUTES_INIT(&attributes);
    attributes.ParentObject = device;
    status = WdfSpinLockCreate(&attributes, &context->OpenLock);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    WDF_IO_QUEUE_CONFIG_INIT_DEFAULT_QUEUE(&queueConfig, WdfIoQueueDispatchParallel);
    queueConfig.EvtIoDeviceControl = MyCtlDeviceControl;
    status = WdfIoQueueCreate(device, &queueConfig, WDF_NO_OBJECT_ATTRIBUTES, WDF_NO_HANDLE);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    status = WdfDeviceCreateDeviceInterface(device, (LPGUID)&GUID_YUMEDISK_CONTROL, NULL);
    return status;
}
