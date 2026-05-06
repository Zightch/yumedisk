#include "transport.h"

#include <ntddscsi.h>

#include "..\core\memory.h"

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
        ioctlBufferSize
    );

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
        DbgPrint(
            "%s ControlProbeMiniportHandle: query failed handle=%p status=%08X\n",
            DRIVER_NAME,
            Handle,
            status);
        return status;
    }

    if (bytesReturned < YUMEDISK_MESSAGE_BASE_SIZE + sizeof(YUMEDISK_QUERY_INFO)) {
        DbgPrint(
            "%s ControlProbeMiniportHandle: short reply handle=%p bytes=%lu\n",
            DRIVER_NAME,
            Handle,
            bytesReturned);
        return STATUS_DEVICE_PROTOCOL_ERROR;
    }

    info = (PYUMEDISK_QUERY_INFO)message->Payload;
    if (RtlCompareMemory(
            info->AdapterSignature,
            YUMEDISK_MINIPORT_SIGNATURE,
            sizeof(info->AdapterSignature)) != sizeof(info->AdapterSignature)) {
        DbgPrint(
            "%s ControlProbeMiniportHandle: signature mismatch handle=%p\n",
            DRIVER_NAME,
            Handle);
        return STATUS_NOT_FOUND;
    }

    DbgPrint(
        "%s ControlProbeMiniportHandle: success handle=%p features=%08X service=%ws\n",
        DRIVER_NAME,
        Handle,
        info->Features,
        info->ServiceName);

    return STATUS_SUCCESS;
}

NTSTATUS
ControlOpenMiniportHandle(
    _Out_ HANDLE* Handle
)
{
    PWSTR interfaces;
    PWSTR current;
    NTSTATUS status;

    *Handle = NULL;
    interfaces = NULL;

    status = IoGetDeviceInterfaces((LPGUID)&ControlStoragePortGuid, NULL, 0, &interfaces);
    if (!NT_SUCCESS(status)) {
        DbgPrint(
            "%s ControlOpenMiniportHandle: IoGetDeviceInterfaces failed status=%08X\n",
            DRIVER_NAME,
            status);
        return status;
    }

    status = STATUS_NO_SUCH_DEVICE;
    current = interfaces;
    DbgPrint(
        "%s ControlOpenMiniportHandle: begin interface scan\n",
        DRIVER_NAME);
    while (*current != UNICODE_NULL) {
        UNICODE_STRING deviceName;
        OBJECT_ATTRIBUTES attributes;
        IO_STATUS_BLOCK ioStatus;
        HANDLE handle;
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

        DbgPrint(
            "%s ControlOpenMiniportHandle: candidate=%wZ openStatus=%08X\n",
            DRIVER_NAME,
            &deviceName,
            openStatus);

        if (NT_SUCCESS(openStatus)) {
            openStatus = ControlProbeMiniportHandle(handle);
            if (NT_SUCCESS(openStatus)) {
                *Handle = handle;
                status = STATUS_SUCCESS;
                DbgPrint(
                    "%s ControlOpenMiniportHandle: selected=%wZ handle=%p\n",
                    DRIVER_NAME,
                    &deviceName,
                    handle);
                break;
            }

            DbgPrint(
                "%s ControlOpenMiniportHandle: probe failed candidate=%wZ status=%08X\n",
                DRIVER_NAME,
                &deviceName,
                openStatus);
            ZwClose(handle);
        }

        current += wcslen(current) + 1;
    }

    ExFreePool(interfaces);
    DbgPrint(
        "%s ControlOpenMiniportHandle: finish status=%08X handle=%p\n",
        DRIVER_NAME,
        status,
        *Handle);
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

VOID
ControlCloseMiniportHandle(
    _Inout_ PCTRL_FILE_CONTEXT Context
)
{
    HANDLE handle;

    if (Context == NULL) {
        return;
    }

    handle = Context->MiniportHandle;
    Context->MiniportHandle = NULL;
    if (handle != NULL) {
        ZwClose(handle);
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
