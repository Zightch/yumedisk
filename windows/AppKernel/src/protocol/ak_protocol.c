#include "protocol/ak_protocol.h"

#include <setupapi.h>

typedef struct AK_PROTOCOL_MESSAGE_BUFFER {
    PYUMEDISK_MESSAGE Message;
    DWORD Size;
} AK_PROTOCOL_MESSAGE_BUFFER;

static AK_STATUS AkProtocolAllocateMessage(
    ULONG command,
    ULONG payload_length,
    ULONG capacity_payload_length,
    AK_PROTOCOL_MESSAGE_BUFFER* out_buffer)
{
    size_t total_size;
    PYUMEDISK_MESSAGE message;

    if (out_buffer == NULL) {
        return AK_STATUS_INVALID_PARAMETER;
    }

    if (capacity_payload_length < payload_length) {
        capacity_payload_length = payload_length;
    }

    total_size = (size_t)YUMEDISK_MESSAGE_BASE_SIZE + (size_t)capacity_payload_length;
    if (total_size > (size_t)MAXDWORD) {
        return AK_STATUS_INVALID_PARAMETER;
    }

    message = (PYUMEDISK_MESSAGE)AkAllocZero(total_size);
    if (message == NULL) {
        return AK_STATUS_INSUFFICIENT_RESOURCES;
    }

    message->Header.Size = (ULONG)total_size;
    message->Header.Version = YUMEDISK_PROTOCOL_VERSION;
    message->Header.Command = command;
    message->Header.Status = AK_STATUS_SUCCESS;
    message->Header.SessionId = 0ull;
    message->Header.TxId = 0ull;
    message->Header.TargetId = 0u;
    message->Header.Flags = 0u;
    message->Header.PayloadLength = payload_length;
    message->Header.Reserved = 0u;

    out_buffer->Message = message;
    out_buffer->Size = (DWORD)total_size;
    return AK_STATUS_SUCCESS;
}

static void AkProtocolFreeMessage(
    AK_PROTOCOL_MESSAGE_BUFFER* buffer)
{
    if (buffer == NULL) {
        return;
    }

    AkFree(buffer->Message);
    buffer->Message = NULL;
    buffer->Size = 0u;
}

static AK_STATUS AkProtocolSendMessage(
    HANDLE file,
    AK_PROTOCOL_MESSAGE_BUFFER* buffer)
{
    DWORD bytes_returned;
    BOOL ok;

    if ((file == NULL) || (file == INVALID_HANDLE_VALUE) || (buffer == NULL) || (buffer->Message == NULL)) {
        return AK_STATUS_INVALID_PARAMETER;
    }

    bytes_returned = 0u;
    ok = DeviceIoControl(
        file,
        IOCTL_YUMEDISK_APP_COMMAND,
        buffer->Message,
        buffer->Size,
        buffer->Message,
        buffer->Size,
        &bytes_returned,
        NULL);
    (void)bytes_returned;

    if (!ok) {
        return AkFromWin32Error(GetLastError());
    }

    return AK_STATUS_SUCCESS;
}

static AK_STATUS AkProtocolValidateInfoResponse(
    const AK_PROTOCOL_MESSAGE_BUFFER* buffer,
    YUMEDISK_QUERY_INFO* out_info,
    UINT64* out_session_id)
{
    if ((buffer == NULL) || (buffer->Message == NULL)) {
        return AK_STATUS_INVALID_PARAMETER;
    }

    if (buffer->Message->Header.Status != AK_STATUS_SUCCESS) {
        return buffer->Message->Header.Status;
    }

    if (buffer->Message->Header.PayloadLength < sizeof(YUMEDISK_QUERY_INFO)) {
        return AK_STATUS_UNSUCCESSFUL;
    }

    if (out_session_id != NULL) {
        if (buffer->Message->Header.SessionId == 0ull) {
            return AK_STATUS_DEVICE_NOT_READY;
        }

        *out_session_id = buffer->Message->Header.SessionId;
    }

    if (out_info != NULL) {
        *out_info = *(const YUMEDISK_QUERY_INFO*)buffer->Message->Payload;
    }

    return AK_STATUS_SUCCESS;
}

static AK_STATUS AkProtocolOpenInterfacePath(
    PCWSTR device_path,
    HANDLE* out_file)
{
    HANDLE file;

    file = CreateFileW(
        device_path,
        GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        NULL,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED,
        NULL);
    if (file == INVALID_HANDLE_VALUE) {
        return AkFromWin32Error(GetLastError());
    }

    *out_file = file;
    return AK_STATUS_SUCCESS;
}

AK_STATUS AkProtocolOpenControlDevice(
    HANDLE* out_file)
{
    HDEVINFO info_set;
    DWORD index;
    DWORD last_error;
    AK_STATUS last_status;

    if (out_file == NULL) {
        return AK_STATUS_INVALID_PARAMETER;
    }

    *out_file = INVALID_HANDLE_VALUE;
    last_error = ERROR_NOT_FOUND;
    last_status = AK_STATUS_NOT_FOUND;

    info_set = SetupDiGetClassDevsW(
        &GUID_YUMEDISK_CONTROL,
        NULL,
        NULL,
        DIGCF_DEVICEINTERFACE | DIGCF_PRESENT);
    if (info_set == INVALID_HANDLE_VALUE) {
        return AkFromWin32Error(GetLastError());
    }

    index = 0u;
    for (;;) {
        SP_DEVICE_INTERFACE_DATA interface_data;
        DWORD required_size;
        PSP_DEVICE_INTERFACE_DETAIL_DATA_W detail_data;
        AK_STATUS status;

        ZeroMemory(&interface_data, sizeof(interface_data));
        interface_data.cbSize = sizeof(interface_data);

        if (!SetupDiEnumDeviceInterfaces(
                info_set,
                NULL,
                &GUID_YUMEDISK_CONTROL,
                index,
                &interface_data)) {
            last_error = GetLastError();
            if (last_error == ERROR_NO_MORE_ITEMS) {
                break;
            }

            last_status = AkFromWin32Error(last_error);
            break;
        }

        required_size = 0u;
        if (!SetupDiGetDeviceInterfaceDetailW(
                info_set,
                &interface_data,
                NULL,
                0u,
                &required_size,
                NULL)) {
            last_error = GetLastError();
            if (last_error != ERROR_INSUFFICIENT_BUFFER) {
                last_status = AkFromWin32Error(last_error);
                break;
            }
        }

        detail_data = (PSP_DEVICE_INTERFACE_DETAIL_DATA_W)AkAllocZero(required_size);
        if (detail_data == NULL) {
            last_status = AK_STATUS_INSUFFICIENT_RESOURCES;
            break;
        }

        detail_data->cbSize = sizeof(*detail_data);
        if (!SetupDiGetDeviceInterfaceDetailW(
                info_set,
                &interface_data,
                detail_data,
                required_size,
                NULL,
                NULL)) {
            last_error = GetLastError();
            last_status = AkFromWin32Error(last_error);
            AkFree(detail_data);
            break;
        }

        status = AkProtocolOpenInterfacePath(detail_data->DevicePath, out_file);
        AkFree(detail_data);

        if (status == AK_STATUS_SUCCESS) {
            last_status = status;
            break;
        }

        last_status = status;
        last_error = ERROR_OPEN_FAILED;
        ++index;
    }

    SetupDiDestroyDeviceInfoList(info_set);
    if (*out_file != INVALID_HANDLE_VALUE) {
        return AK_STATUS_SUCCESS;
    }

    SetLastError(last_error);
    return last_status;
}

AK_STATUS AkProtocolQueryInfo(
    HANDLE file,
    YUMEDISK_QUERY_INFO* out_info,
    UINT64* out_session_id)
{
    AK_PROTOCOL_MESSAGE_BUFFER buffer;
    AK_STATUS status;

    status = AkProtocolAllocateMessage(
        YumeDiskCommandQueryInfo,
        0u,
        (ULONG)sizeof(YUMEDISK_QUERY_INFO),
        &buffer);
    if (status != AK_STATUS_SUCCESS) {
        return status;
    }

    status = AkProtocolSendMessage(file, &buffer);
    if (status == AK_STATUS_SUCCESS) {
        status = AkProtocolValidateInfoResponse(&buffer, out_info, out_session_id);
    }

    AkProtocolFreeMessage(&buffer);
    return status;
}

AK_STATUS AkProtocolQuerySessionId(
    HANDLE file,
    UINT64* out_session_id)
{
    return AkProtocolQueryInfo(file, NULL, out_session_id);
}

AK_STATUS AkProtocolSendHeartbeat(
    HANDLE file,
    UINT64 session_id)
{
    AK_PROTOCOL_MESSAGE_BUFFER buffer;
    AK_STATUS status;

    status = AkProtocolAllocateMessage(
        YumeDiskCommandHeartbeat,
        0u,
        0u,
        &buffer);
    if (status != AK_STATUS_SUCCESS) {
        return status;
    }

    buffer.Message->Header.SessionId = session_id;

    status = AkProtocolSendMessage(file, &buffer);
    if ((status == AK_STATUS_SUCCESS) && (buffer.Message->Header.Status != AK_STATUS_SUCCESS)) {
        status = buffer.Message->Header.Status;
    }

    AkProtocolFreeMessage(&buffer);
    return status;
}

AK_STATUS AkProtocolRemoveAllDisks(
    HANDLE file,
    UINT64 session_id,
    ULONG flags)
{
    AK_PROTOCOL_MESSAGE_BUFFER buffer;
    AK_STATUS status;

    status = AkProtocolAllocateMessage(
        YumeDiskCommandRemoveAllDisks,
        0u,
        0u,
        &buffer);
    if (status != AK_STATUS_SUCCESS) {
        return status;
    }

    buffer.Message->Header.SessionId = session_id;
    buffer.Message->Header.Flags = flags;

    status = AkProtocolSendMessage(file, &buffer);
    if ((status == AK_STATUS_SUCCESS) && (buffer.Message->Header.Status != AK_STATUS_SUCCESS)) {
        status = buffer.Message->Header.Status;
    }

    AkProtocolFreeMessage(&buffer);
    return status;
}

AK_STATUS AkProtocolUnavailable(void)
{
    return AK_STATUS_NOT_SUPPORTED;
}
