#pragma once

#include "common/ak_internal.h"

typedef struct AK_PROTOCOL_MESSAGE_BUFFER {
    PYUMEDISK_MESSAGE Message;
    DWORD Size;
} AK_PROTOCOL_MESSAGE_BUFFER;

typedef struct AK_PROTOCOL_ASYNC_IO {
    OVERLAPPED Overlapped;
    DWORD BytesTransferred;
    BOOLEAN Active;
} AK_PROTOCOL_ASYNC_IO;

AK_STATUS AkProtocolMessageAllocate(
    ULONG command,
    ULONG payload_length,
    ULONG capacity_payload_length,
    AK_PROTOCOL_MESSAGE_BUFFER* out_buffer);

void AkProtocolMessageRelease(
    AK_PROTOCOL_MESSAGE_BUFFER* buffer);

AK_STATUS AkProtocolMessageReset(
    AK_PROTOCOL_MESSAGE_BUFFER* buffer,
    ULONG command,
    ULONG payload_length,
    UINT64 session_id,
    UINT64 tx_id,
    ULONG target_id,
    ULONG flags);

ULONG AkProtocolWriteAckBatchPayloadSize(
    UINT32 range_count);

AK_STATUS AkProtocolPreparePostReadSlot(
    AK_PROTOCOL_MESSAGE_BUFFER* request,
    UINT64 session_id,
    UINT32 target_id,
    UINT64 tx_id);

AK_STATUS AkProtocolPreparePostWriteSlot(
    AK_PROTOCOL_MESSAGE_BUFFER* request,
    UINT64 session_id,
    UINT32 target_id,
    UINT64 tx_id);

AK_STATUS AkProtocolPrepareReadAck(
    AK_PROTOCOL_MESSAGE_BUFFER* request,
    UINT64 session_id,
    UINT32 target_id,
    const YUMEDISK_READ_ACK* ack);

AK_STATUS AkProtocolPrepareWriteAckBatch(
    AK_PROTOCOL_MESSAGE_BUFFER* request,
    UINT64 session_id,
    UINT32 target_id,
    const YUMEDISK_WRITE_ACK_RANGE* ranges,
    UINT32 range_count);

AK_STATUS AkProtocolAsyncIoInitialize(
    AK_PROTOCOL_ASYNC_IO* async_io);

void AkProtocolAsyncIoDestroy(
    AK_PROTOCOL_ASYNC_IO* async_io);

AK_STATUS AkProtocolAsyncIoBegin(
    HANDLE file,
    void* input_buffer,
    DWORD input_buffer_size,
    void* output_buffer,
    DWORD output_buffer_size,
    AK_PROTOCOL_ASYNC_IO* async_io);

AK_STATUS AkProtocolAsyncIoWait(
    AK_PROTOCOL_ASYNC_IO* async_io,
    DWORD timeout_ms);

AK_STATUS AkProtocolAsyncIoFinish(
    HANDLE file,
    AK_PROTOCOL_ASYNC_IO* async_io,
    DWORD* out_bytes_transferred);

AK_STATUS AkProtocolAsyncIoCancel(
    HANDLE file,
    AK_PROTOCOL_ASYNC_IO* async_io);

AK_STATUS AkProtocolOpenControlDevice(
    HANDLE* out_file);

AK_STATUS AkProtocolQueryKmdfInfo(
    HANDLE file,
    YUMEDISK_KMDF_INFO* out_info,
    UINT64* out_session_id);

AK_STATUS AkProtocolQueryScsiInfo(
    HANDLE file,
    YUMEDISK_SCSI_INFO* out_info);

AK_STATUS AkProtocolQuerySessionId(
    HANDLE file,
    UINT64* out_session_id);

AK_STATUS AkProtocolSendHeartbeat(
    HANDLE file,
    UINT64 session_id);

AK_STATUS AkProtocolRemoveAllDisks(
    HANDLE file,
    UINT64 session_id,
    ULONG flags);

AK_STATUS AkProtocolCreateDisk(
    HANDLE file,
    UINT64 session_id,
    const AK_DISK_PARAMS* params);

AK_STATUS AkProtocolRemoveDisk(
    HANDLE file,
    UINT64 session_id,
    UINT32 target_id,
    ULONG flags);

AK_STATUS AkProtocolNotifyDataChanged(
    HANDLE file,
    UINT64 session_id,
    UINT32 target_id);

AK_STATUS AkProtocolQueryDebugState(
    HANDLE file,
    UINT64 session_id,
    YUMEDISK_DEBUG_STATE* out_debug_state);

AK_STATUS AkProtocolCancelSlot(
    HANDLE file,
    UINT64 session_id,
    UINT32 target_id,
    UINT32 slot_type,
    UINT64 slot_id);

AK_STATUS AkProtocolUnavailable(void);
