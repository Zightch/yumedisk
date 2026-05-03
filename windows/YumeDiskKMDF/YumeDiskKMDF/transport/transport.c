#include "transport.h"

#include <ntddscsi.h>

#include "..\core\memory.h"

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

static
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
            FILE_NON_DIRECTORY_FILE | FILE_SYNCHRONOUS_IO_NONALERT,
            NULL,
            0);

        if (NT_SUCCESS(openStatus)) {
            openStatus = ControlProbeMiniportHandle(handle);
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

NTSTATUS
ControlProxyCommand(
    _Inout_updates_bytes_(BufferCapacity) PUCHAR Buffer,
    _In_ ULONG InputLength,
    _In_ ULONG BufferCapacity,
    _Out_ ULONG* BytesReturned
)
{
    HANDLE handle;
    NTSTATUS status;

    handle = NULL;
    status = ControlOpenMiniportHandle(&handle);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    status = ControlSendMiniportBuffer(handle, Buffer, InputLength, BufferCapacity, BytesReturned);
    ZwClose(handle);
    return status;
}

VOID
ControlSendSessionCleanup(
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

    (VOID)ControlProxyCommand(buffer, sizeof(buffer), sizeof(buffer), &bytesReturned);
}

