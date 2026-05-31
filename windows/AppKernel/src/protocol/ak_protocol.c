#include "protocol/ak_protocol.h"

#include <setupapi.h>
#include <string.h>

static AK_STATUS AkProtocolValidateMessageBuffer(
    const AK_PROTOCOL_MESSAGE_BUFFER* buffer,
    ULONG payload_length)
{
    size_t minimum_size;

    if ((buffer == NULL) || (buffer->Message == NULL)) {
        return AK_STATUS_INVALID_PARAMETER;
    }

    minimum_size = (size_t)YUMEDISK_MESSAGE_BASE_SIZE + (size_t)payload_length;
    if ((size_t)buffer->Size < minimum_size) {
        return AK_STATUS_INVALID_PARAMETER;
    }

    return AK_STATUS_SUCCESS;
}

AK_STATUS AkProtocolMessageAllocate(
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

    out_buffer->Message = NULL;
    out_buffer->Size = 0u;

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
    message->Header.Command = command;
    message->Header.Status = AK_STATUS_SUCCESS;
    message->Header.Reserved0 = 0u;
    message->Header.SessionId = 0ull;
    message->Header.TxId = 0ull;
    message->Header.TargetId = 0u;
    message->Header.Flags = 0u;
    message->Header.PayloadLength = payload_length;
    message->Header.Reserved1 = 0u;

    out_buffer->Message = message;
    out_buffer->Size = (DWORD)total_size;
    return AK_STATUS_SUCCESS;
}

void AkProtocolMessageRelease(
    AK_PROTOCOL_MESSAGE_BUFFER* buffer)
{
    if (buffer == NULL) {
        return;
    }

    AkFree(buffer->Message);
    buffer->Message = NULL;
    buffer->Size = 0u;
}

AK_STATUS AkProtocolMessageReset(
    AK_PROTOCOL_MESSAGE_BUFFER* buffer,
    ULONG command,
    ULONG payload_length,
    UINT64 session_id,
    UINT64 tx_id,
    ULONG target_id,
    ULONG flags)
{
    AK_STATUS status;

    status = AkProtocolValidateMessageBuffer(buffer, payload_length);
    if (status != AK_STATUS_SUCCESS) {
        return status;
    }

    (void)memset(buffer->Message, 0, buffer->Size);
    buffer->Message->Header.Size = buffer->Size;
    buffer->Message->Header.Command = command;
    buffer->Message->Header.Status = AK_STATUS_SUCCESS;
    buffer->Message->Header.Reserved0 = 0u;
    buffer->Message->Header.SessionId = session_id;
    buffer->Message->Header.TxId = tx_id;
    buffer->Message->Header.TargetId = target_id;
    buffer->Message->Header.Flags = flags;
    buffer->Message->Header.PayloadLength = payload_length;
    buffer->Message->Header.Reserved1 = 0u;
    return AK_STATUS_SUCCESS;
}

ULONG AkProtocolWriteAckBatchPayloadSize(
    UINT32 range_count)
{
    if (range_count == 0u) {
        return 0u;
    }

    return YUMEDISK_WRITE_ACK_BATCH_SIZE(range_count);
}

AK_STATUS AkProtocolPreparePostReadSlot(
    AK_PROTOCOL_MESSAGE_BUFFER* request,
    UINT64 session_id,
    UINT32 target_id,
    UINT64 tx_id)
{
    return AkProtocolMessageReset(
        request,
        YumeDiskCommandPostReadSlot,
        0u,
        session_id,
        tx_id,
        target_id,
        0u);
}

AK_STATUS AkProtocolPreparePostWriteSlot(
    AK_PROTOCOL_MESSAGE_BUFFER* request,
    UINT64 session_id,
    UINT32 target_id,
    UINT64 tx_id)
{
    return AkProtocolMessageReset(
        request,
        YumeDiskCommandPostWriteSlot,
        0u,
        session_id,
        tx_id,
        target_id,
        0u);
}

AK_STATUS AkProtocolPreparePostEventSlot(
    AK_PROTOCOL_MESSAGE_BUFFER* request,
    UINT64 session_id,
    UINT32 target_id,
    UINT64 tx_id)
{
    return AkProtocolMessageReset(
        request,
        YumeDiskCommandPostEventSlot,
        0u,
        session_id,
        tx_id,
        target_id,
        0u);
}

AK_STATUS AkProtocolPrepareReadAck(
    AK_PROTOCOL_MESSAGE_BUFFER* request,
    UINT64 session_id,
    UINT32 target_id,
    const YUMEDISK_READ_ACK* ack)
{
    YUMEDISK_READ_ACK* payload;
    AK_STATUS status;

    if (ack == NULL) {
        return AK_STATUS_INVALID_PARAMETER;
    }

    status = AkProtocolMessageReset(
        request,
        YumeDiskCommandReadAck,
        (ULONG)sizeof(YUMEDISK_READ_ACK),
        session_id,
        0ull,
        target_id,
        0u);
    if (status != AK_STATUS_SUCCESS) {
        return status;
    }

    payload = (YUMEDISK_READ_ACK*)request->Message->Payload;
    *payload = *ack;
    return AK_STATUS_SUCCESS;
}

AK_STATUS AkProtocolPrepareWriteAckBatch(
    AK_PROTOCOL_MESSAGE_BUFFER* request,
    UINT64 session_id,
    UINT32 target_id,
    const YUMEDISK_WRITE_ACK_RANGE* ranges,
    UINT32 range_count)
{
    ULONG payload_length;
    PYUMEDISK_WRITE_ACK_BATCH payload;
    AK_STATUS status;

    payload_length = AkProtocolWriteAckBatchPayloadSize(range_count);
    status = AkProtocolMessageReset(
        request,
        YumeDiskCommandWriteAckBatch,
        payload_length,
        session_id,
        0ull,
        target_id,
        0u);
    if (status != AK_STATUS_SUCCESS) {
        return status;
    }

    if (range_count == 0u) {
        return AK_STATUS_SUCCESS;
    }

    if (ranges == NULL) {
        return AK_STATUS_INVALID_PARAMETER;
    }

    payload = (PYUMEDISK_WRITE_ACK_BATCH)request->Message->Payload;
    payload->RangeCount = range_count;
    payload->Reserved = 0u;
    (void)memcpy(
        payload->Ranges,
        ranges,
        (size_t)range_count * sizeof(YUMEDISK_WRITE_ACK_RANGE));
    return AK_STATUS_SUCCESS;
}

AK_STATUS AkProtocolAsyncIoInitialize(
    AK_PROTOCOL_ASYNC_IO* async_io)
{
    if (async_io == NULL) {
        return AK_STATUS_INVALID_PARAMETER;
    }

    (void)memset(async_io, 0, sizeof(*async_io));
    async_io->Overlapped.hEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (async_io->Overlapped.hEvent == NULL) {
        return AkFromWin32Error(GetLastError());
    }

    async_io->BytesTransferred = 0u;
    async_io->Active = FALSE;
    return AK_STATUS_SUCCESS;
}

void AkProtocolAsyncIoDestroy(
    AK_PROTOCOL_ASYNC_IO* async_io)
{
    if (async_io == NULL) {
        return;
    }

    if (async_io->Overlapped.hEvent != NULL) {
        CloseHandle(async_io->Overlapped.hEvent);
        async_io->Overlapped.hEvent = NULL;
    }

    async_io->BytesTransferred = 0u;
    async_io->Active = FALSE;
}

AK_STATUS AkProtocolAsyncIoBegin(
    HANDLE file,
    void* input_buffer,
    DWORD input_buffer_size,
    void* output_buffer,
    DWORD output_buffer_size,
    AK_PROTOCOL_ASYNC_IO* async_io)
{
    HANDLE event_handle;
    OVERLAPPED reset_overlapped;
    DWORD transferred;
    BOOL ok;
    DWORD error;

    if ((file == NULL) || (file == INVALID_HANDLE_VALUE) || (async_io == NULL) ||
        (async_io->Overlapped.hEvent == NULL)) {
        return AK_STATUS_INVALID_PARAMETER;
    }

    event_handle = async_io->Overlapped.hEvent;
    (void)memset(&reset_overlapped, 0, sizeof(reset_overlapped));
    reset_overlapped.hEvent = event_handle;
    async_io->Overlapped = reset_overlapped;
    async_io->BytesTransferred = 0u;
    async_io->Active = FALSE;
    ResetEvent(event_handle);

    transferred = 0u;
    ok = DeviceIoControl(
        file,
        IOCTL_YUMEDISK_APP_COMMAND,
        input_buffer,
        input_buffer_size,
        output_buffer,
        output_buffer_size,
        &transferred,
        &async_io->Overlapped);
    if (ok) {
        async_io->BytesTransferred = transferred;
        async_io->Active = TRUE;
        SetEvent(event_handle);
        return AK_STATUS_SUCCESS;
    }

    error = GetLastError();
    if (error == ERROR_IO_PENDING) {
        async_io->Active = TRUE;
        return AK_STATUS_SUCCESS;
    }

    return AkFromWin32Error(error);
}

AK_STATUS AkProtocolAsyncIoWait(
    AK_PROTOCOL_ASYNC_IO* async_io,
    DWORD timeout_ms)
{
    DWORD wait_status;

    if ((async_io == NULL) || (async_io->Overlapped.hEvent == NULL)) {
        return AK_STATUS_INVALID_PARAMETER;
    }

    wait_status = WaitForSingleObject(async_io->Overlapped.hEvent, timeout_ms);
    if (wait_status == WAIT_OBJECT_0) {
        return AK_STATUS_SUCCESS;
    }

    if (wait_status == WAIT_TIMEOUT) {
        return AK_STATUS_TIMEOUT;
    }

    return AkFromWin32Error(GetLastError());
}

AK_STATUS AkProtocolAsyncIoFinish(
    HANDLE file,
    AK_PROTOCOL_ASYNC_IO* async_io,
    DWORD* out_bytes_transferred)
{
    DWORD transferred;
    BOOL ok;
    DWORD error;

    if ((file == NULL) || (file == INVALID_HANDLE_VALUE) || (async_io == NULL) ||
        (async_io->Overlapped.hEvent == NULL) || !async_io->Active) {
        return AK_STATUS_INVALID_PARAMETER;
    }

    transferred = 0u;
    ok = GetOverlappedResult(file, &async_io->Overlapped, &transferred, FALSE);
    if (ok) {
        async_io->BytesTransferred = transferred;
        async_io->Active = FALSE;
        if (out_bytes_transferred != NULL) {
            *out_bytes_transferred = transferred;
        }
        return AK_STATUS_SUCCESS;
    }

    error = GetLastError();
    if (error == ERROR_IO_INCOMPLETE) {
        return AK_STATUS_TIMEOUT;
    }

    async_io->Active = FALSE;
    return AkFromWin32Error(error);
}

AK_STATUS AkProtocolAsyncIoCancel(
    HANDLE file,
    AK_PROTOCOL_ASYNC_IO* async_io)
{
    BOOL ok;
    DWORD error;

    if ((file == NULL) || (file == INVALID_HANDLE_VALUE) || (async_io == NULL) ||
        (async_io->Overlapped.hEvent == NULL)) {
        return AK_STATUS_INVALID_PARAMETER;
    }

    ok = CancelIoEx(file, &async_io->Overlapped);
    if (ok) {
        return AK_STATUS_SUCCESS;
    }

    error = GetLastError();
    if (error == ERROR_NOT_FOUND) {
        return AK_STATUS_NOT_FOUND;
    }

    return AkFromWin32Error(error);
}

static AK_STATUS AkProtocolSendMessage(
    HANDLE file,
    AK_PROTOCOL_MESSAGE_BUFFER* buffer)
{
    OVERLAPPED overlapped;
    HANDLE event_handle;
    DWORD bytes_returned;
    BOOL ok;
    DWORD error;
    AK_STATUS status;

    if ((file == NULL) || (file == INVALID_HANDLE_VALUE) || (buffer == NULL) || (buffer->Message == NULL)) {
        return AK_STATUS_INVALID_PARAMETER;
    }

    event_handle = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (event_handle == NULL) {
        return AkFromWin32Error(GetLastError());
    }

    (void)memset(&overlapped, 0, sizeof(overlapped));
    overlapped.hEvent = event_handle;
    bytes_returned = 0u;
    error = ERROR_SUCCESS;
    ok = DeviceIoControl(
        file,
        IOCTL_YUMEDISK_APP_COMMAND,
        buffer->Message,
        buffer->Size,
        buffer->Message,
        buffer->Size,
        &bytes_returned,
        &overlapped);
    if (!ok) {
        error = GetLastError();
        if (error == ERROR_IO_PENDING) {
            ok = GetOverlappedResult(file, &overlapped, &bytes_returned, TRUE);
            if (!ok) {
                error = GetLastError();
            }
        }
    }

    status = AK_STATUS_SUCCESS;
    if (!ok) {
        status = AkFromWin32Error(error);
    }

    CloseHandle(event_handle);
    return status;
}

static AK_STATUS AkProtocolSendShortCommand(
    HANDLE file,
    AK_PROTOCOL_MESSAGE_BUFFER* buffer)
{
    AK_STATUS status;

    status = AkProtocolSendMessage(file, buffer);
    if (status != AK_STATUS_SUCCESS) {
        return status;
    }

    if (buffer->Message->Header.Status != AK_STATUS_SUCCESS) {
        return buffer->Message->Header.Status;
    }

    return AK_STATUS_SUCCESS;
}

static AK_STATUS AkProtocolValidateKmdfInfoResponse(
    const AK_PROTOCOL_MESSAGE_BUFFER* buffer,
    YUMEDISK_KMDF_INFO* out_info,
    UINT64* out_session_id)
{
    if ((buffer == NULL) || (buffer->Message == NULL)) {
        return AK_STATUS_INVALID_PARAMETER;
    }

    if (buffer->Message->Header.Status != AK_STATUS_SUCCESS) {
        return buffer->Message->Header.Status;
    }

    if (buffer->Message->Header.PayloadLength < sizeof(YUMEDISK_KMDF_INFO)) {
        return AK_STATUS_UNSUCCESSFUL;
    }

    if (out_session_id != NULL) {
        if (buffer->Message->Header.SessionId == 0ull) {
            return AK_STATUS_DEVICE_NOT_READY;
        }

        *out_session_id = buffer->Message->Header.SessionId;
    }

    if (out_info != NULL) {
        *out_info = *(const YUMEDISK_KMDF_INFO*)buffer->Message->Payload;
    }

    return AK_STATUS_SUCCESS;
}

static AK_STATUS AkProtocolValidateScsiInfoResponse(
    const AK_PROTOCOL_MESSAGE_BUFFER* buffer,
    YUMEDISK_SCSI_INFO* out_info)
{
    if ((buffer == NULL) || (buffer->Message == NULL) || (out_info == NULL)) {
        return AK_STATUS_INVALID_PARAMETER;
    }

    if (buffer->Message->Header.Status != AK_STATUS_SUCCESS) {
        return buffer->Message->Header.Status;
    }

    if (buffer->Message->Header.PayloadLength < sizeof(YUMEDISK_SCSI_INFO)) {
        return AK_STATUS_UNSUCCESSFUL;
    }

    *out_info = *(const YUMEDISK_SCSI_INFO*)buffer->Message->Payload;

    return AK_STATUS_SUCCESS;
}

static AK_STATUS AkProtocolOpenInterfacePath(
    PCWSTR device_path,
    HANDLE* out_file,
    DWORD* out_error)
{
    HANDLE file;

    file = CreateFileW(
        device_path,
        GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        NULL,
        OPEN_EXISTING,
        FILE_FLAG_OVERLAPPED,
        NULL);
    if (file == INVALID_HANDLE_VALUE) {
        DWORD error;

        error = GetLastError();
        if (out_error != NULL) {
            *out_error = error;
        }

        return AkFromWin32Error(error);
    }

    if (out_error != NULL) {
        *out_error = ERROR_SUCCESS;
    }

    *out_file = file;
    return AK_STATUS_SUCCESS;
}

AK_STATUS AkProtocolOpenControlDevice(
    HANDLE* out_file)
{
    const ULONGLONG timeout_ms = 2000ull;
    const DWORD retry_delay_ms = 100u;
    const ULONGLONG deadline = GetTickCount64() + timeout_ms;
    DWORD last_error;
    AK_STATUS last_status;

    if (out_file == NULL) {
        return AK_STATUS_INVALID_PARAMETER;
    }

    *out_file = INVALID_HANDLE_VALUE;
    last_error = ERROR_NOT_FOUND;
    last_status = AK_STATUS_NOT_FOUND;

    for (;;) {
        HDEVINFO info_set;
        DWORD index;

        info_set = SetupDiGetClassDevsW(
            &GUID_YUMEDISK_CONTROL,
            NULL,
            NULL,
            DIGCF_DEVICEINTERFACE | DIGCF_PRESENT);
        if (info_set == INVALID_HANDLE_VALUE) {
            last_error = GetLastError();
            last_status = AkFromWin32Error(last_error);
        } else {
            index = 0u;
            for (;;) {
                SP_DEVICE_INTERFACE_DATA interface_data;
                DWORD required_size;
                PSP_DEVICE_INTERFACE_DETAIL_DATA_W detail_data;
                DWORD open_error;
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
                    last_error = ERROR_OUTOFMEMORY;
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

                open_error = ERROR_SUCCESS;
                status = AkProtocolOpenInterfacePath(detail_data->DevicePath, out_file, &open_error);
                AkFree(detail_data);

                if (status == AK_STATUS_SUCCESS) {
                    last_status = status;
                    last_error = ERROR_SUCCESS;
                    break;
                }

                last_status = status;
                last_error = open_error;
                ++index;
            }

            SetupDiDestroyDeviceInfoList(info_set);
        }

        if (*out_file != INVALID_HANDLE_VALUE) {
            return AK_STATUS_SUCCESS;
        }

        if ((last_status != AK_STATUS_NOT_FOUND) ||
            (GetTickCount64() >= deadline)) {
            break;
        }

        Sleep(retry_delay_ms);
    }

    SetLastError(last_error);
    return last_status;
}

AK_STATUS AkProtocolQueryKmdfInfo(
    HANDLE file,
    YUMEDISK_KMDF_INFO* out_info,
    UINT64* out_session_id)
{
    AK_PROTOCOL_MESSAGE_BUFFER buffer;
    AK_STATUS status;

    status = AkProtocolMessageAllocate(
        YumeDiskCommandQueryKmdfInfo,
        0u,
        (ULONG)sizeof(YUMEDISK_KMDF_INFO),
        &buffer);
    if (status != AK_STATUS_SUCCESS) {
        return status;
    }

    status = AkProtocolSendMessage(file, &buffer);
    if (status == AK_STATUS_SUCCESS) {
        status = AkProtocolValidateKmdfInfoResponse(&buffer, out_info, out_session_id);
    }

    AkProtocolMessageRelease(&buffer);
    return status;
}

AK_STATUS AkProtocolQueryScsiInfo(
    HANDLE file,
    YUMEDISK_SCSI_INFO* out_info)
{
    AK_PROTOCOL_MESSAGE_BUFFER buffer;
    AK_STATUS status;

    if (out_info == NULL) {
        return AK_STATUS_INVALID_PARAMETER;
    }

    status = AkProtocolMessageAllocate(
        YumeDiskCommandQueryScsiInfo,
        0u,
        (ULONG)sizeof(YUMEDISK_SCSI_INFO),
        &buffer);
    if (status != AK_STATUS_SUCCESS) {
        return status;
    }

    status = AkProtocolSendMessage(file, &buffer);
    if (status == AK_STATUS_SUCCESS) {
        status = AkProtocolValidateScsiInfoResponse(&buffer, out_info);
    }

    AkProtocolMessageRelease(&buffer);
    return status;
}

AK_STATUS AkProtocolQuerySessionId(
    HANDLE file,
    UINT64* out_session_id)
{
    return AkProtocolQueryKmdfInfo(file, NULL, out_session_id);
}

AK_STATUS AkProtocolSendHeartbeat(
    HANDLE file,
    UINT64 session_id)
{
    AK_PROTOCOL_MESSAGE_BUFFER buffer;
    AK_STATUS status;

    status = AkProtocolMessageAllocate(
        YumeDiskCommandHeartbeat,
        0u,
        0u,
        &buffer);
    if (status != AK_STATUS_SUCCESS) {
        return status;
    }

    status = AkProtocolMessageReset(
        &buffer,
        YumeDiskCommandHeartbeat,
        0u,
        session_id,
        0ull,
        0u,
        0u);
    if (status != AK_STATUS_SUCCESS) {
        AkProtocolMessageRelease(&buffer);
        return status;
    }

    status = AkProtocolSendShortCommand(file, &buffer);

    AkProtocolMessageRelease(&buffer);
    return status;
}

AK_STATUS AkProtocolRemoveAllDisks(
    HANDLE file,
    UINT64 session_id,
    ULONG flags)
{
    AK_PROTOCOL_MESSAGE_BUFFER buffer;
    AK_STATUS status;

    status = AkProtocolMessageAllocate(
        YumeDiskCommandRemoveAllDisks,
        0u,
        0u,
        &buffer);
    if (status != AK_STATUS_SUCCESS) {
        return status;
    }

    status = AkProtocolMessageReset(
        &buffer,
        YumeDiskCommandRemoveAllDisks,
        0u,
        session_id,
        0ull,
        0u,
        flags);
    if (status != AK_STATUS_SUCCESS) {
        AkProtocolMessageRelease(&buffer);
        return status;
    }

    status = AkProtocolSendShortCommand(file, &buffer);

    AkProtocolMessageRelease(&buffer);
    return status;
}

AK_STATUS AkProtocolCreateDisk(
    HANDLE file,
    UINT64 session_id,
    const AK_DISK_PARAMS* params)
{
    AK_PROTOCOL_MESSAGE_BUFFER buffer;
    YUMEDISK_CREATE_DISK* payload;
    AK_STATUS status;

    if (params == NULL) {
        return AK_STATUS_INVALID_PARAMETER;
    }

    if ((params->SectorSize == 0u) || (params->DiskSizeBytes == 0ull) ||
        ((params->DiskSizeBytes % params->SectorSize) != 0ull)) {
        return AK_STATUS_INVALID_PARAMETER;
    }

    status = AkProtocolMessageAllocate(
        YumeDiskCommandCreateDisk,
        (ULONG)sizeof(YUMEDISK_CREATE_DISK),
        (ULONG)sizeof(YUMEDISK_CREATE_DISK),
        &buffer);
    if (status != AK_STATUS_SUCCESS) {
        return status;
    }

    status = AkProtocolMessageReset(
        &buffer,
        YumeDiskCommandCreateDisk,
        (ULONG)sizeof(YUMEDISK_CREATE_DISK),
        session_id,
        0ull,
        params->TargetId,
        0u);
    if (status != AK_STATUS_SUCCESS) {
        AkProtocolMessageRelease(&buffer);
        return status;
    }

    payload = (YUMEDISK_CREATE_DISK*)buffer.Message->Payload;
    payload->TargetId = params->TargetId;
    payload->SectorSize = params->SectorSize;
    payload->ReadOnly = params->ReadOnly != 0u ? 1u : 0u;
    payload->Reserved0 = 0u;
    payload->SectorCount = params->DiskSizeBytes / params->SectorSize;
    payload->DiskSizeBytes = params->DiskSizeBytes;

    status = AkProtocolSendShortCommand(file, &buffer);
    AkProtocolMessageRelease(&buffer);
    return status;
}

AK_STATUS AkProtocolRemoveDisk(
    HANDLE file,
    UINT64 session_id,
    UINT32 target_id,
    ULONG flags)
{
    AK_PROTOCOL_MESSAGE_BUFFER buffer;
    YUMEDISK_REMOVE_DISK* payload;
    AK_STATUS status;

    status = AkProtocolMessageAllocate(
        YumeDiskCommandRemoveDisk,
        (ULONG)sizeof(YUMEDISK_REMOVE_DISK),
        (ULONG)sizeof(YUMEDISK_REMOVE_DISK),
        &buffer);
    if (status != AK_STATUS_SUCCESS) {
        return status;
    }

    status = AkProtocolMessageReset(
        &buffer,
        YumeDiskCommandRemoveDisk,
        (ULONG)sizeof(YUMEDISK_REMOVE_DISK),
        session_id,
        0ull,
        target_id,
        0u);
    if (status != AK_STATUS_SUCCESS) {
        AkProtocolMessageRelease(&buffer);
        return status;
    }

    payload = (YUMEDISK_REMOVE_DISK*)buffer.Message->Payload;
    payload->TargetId = target_id;
    payload->Flags = flags;

    status = AkProtocolSendShortCommand(file, &buffer);
    AkProtocolMessageRelease(&buffer);
    return status;
}

AK_STATUS AkProtocolNotifyDataChanged(
    HANDLE file,
    UINT64 session_id,
    UINT32 target_id)
{
    AK_PROTOCOL_MESSAGE_BUFFER buffer;
    AK_STATUS status;

    if (target_id > YUMEDISK_MAX_USABLE_TARGET_ID) {
        return AK_STATUS_INVALID_PARAMETER;
    }

    status = AkProtocolMessageAllocate(
        YumeDiskCommandNotifyDataChanged,
        0u,
        0u,
        &buffer);
    if (status != AK_STATUS_SUCCESS) {
        return status;
    }

    status = AkProtocolMessageReset(
        &buffer,
        YumeDiskCommandNotifyDataChanged,
        0u,
        session_id,
        0ull,
        target_id,
        0u);
    if (status != AK_STATUS_SUCCESS) {
        AkProtocolMessageRelease(&buffer);
        return status;
    }

    status = AkProtocolSendShortCommand(file, &buffer);
    AkProtocolMessageRelease(&buffer);
    return status;
}

AK_STATUS AkProtocolQueryDebugState(
    HANDLE file,
    UINT64 session_id,
    YUMEDISK_DEBUG_STATE* out_debug_state)
{
    AK_PROTOCOL_MESSAGE_BUFFER buffer;
    AK_STATUS status;

    if (out_debug_state == NULL) {
        return AK_STATUS_INVALID_PARAMETER;
    }

    status = AkProtocolMessageAllocate(
        YumeDiskCommandQueryDebugState,
        0u,
        (ULONG)sizeof(YUMEDISK_DEBUG_STATE),
        &buffer);
    if (status != AK_STATUS_SUCCESS) {
        return status;
    }

    status = AkProtocolMessageReset(
        &buffer,
        YumeDiskCommandQueryDebugState,
        0u,
        session_id,
        0ull,
        0u,
        0u);
    if (status != AK_STATUS_SUCCESS) {
        AkProtocolMessageRelease(&buffer);
        return status;
    }

    status = AkProtocolSendShortCommand(file, &buffer);
    if (status == AK_STATUS_SUCCESS) {
        if (buffer.Message->Header.PayloadLength < sizeof(YUMEDISK_DEBUG_STATE)) {
            status = AK_STATUS_UNSUCCESSFUL;
        } else {
            *out_debug_state = *(const YUMEDISK_DEBUG_STATE*)buffer.Message->Payload;
        }
    }

    AkProtocolMessageRelease(&buffer);
    return status;
}

AK_STATUS AkProtocolCancelSlot(
    HANDLE file,
    UINT64 session_id,
    UINT32 target_id,
    UINT32 slot_type,
    UINT64 slot_id)
{
    AK_PROTOCOL_MESSAGE_BUFFER buffer;
    YUMEDISK_CANCEL_SLOT* payload;
    AK_STATUS status;

    status = AkProtocolMessageAllocate(
        YumeDiskCommandCancelSlot,
        (ULONG)sizeof(YUMEDISK_CANCEL_SLOT),
        (ULONG)sizeof(YUMEDISK_CANCEL_SLOT),
        &buffer);
    if (status != AK_STATUS_SUCCESS) {
        return status;
    }

    status = AkProtocolMessageReset(
        &buffer,
        YumeDiskCommandCancelSlot,
        (ULONG)sizeof(YUMEDISK_CANCEL_SLOT),
        session_id,
        0ull,
        target_id,
        0u);
    if (status != AK_STATUS_SUCCESS) {
        AkProtocolMessageRelease(&buffer);
        return status;
    }

    payload = (YUMEDISK_CANCEL_SLOT*)buffer.Message->Payload;
    payload->SlotId = slot_id;
    payload->SlotType = slot_type;
    payload->TargetId = target_id;

    status = AkProtocolSendShortCommand(file, &buffer);
    if (status == AK_STATUS_NOT_FOUND) {
        status = AK_STATUS_SUCCESS;
    }

    AkProtocolMessageRelease(&buffer);
    return status;
}

AK_STATUS AkProtocolUnavailable(void)
{
    return AK_STATUS_NOT_SUPPORTED;
}
