#pragma once

#include <windows.h>
#include "yumedisk_proto.h"

#ifdef __cplusplus
extern "C" {
#endif

#if defined(AK_BUILD_DLL)
#define AK_API __declspec(dllexport)
#else
#define AK_API __declspec(dllimport)
#endif

#define AK_CALL __cdecl

#define AK_VERSION_BE YUMEDISK_COMPONENT_VERSION_BE

typedef struct AK_SESSION AK_SESSION;
typedef struct AK_DISK AK_DISK;

typedef LONG AK_STATUS;

#define AK_STATUS_SUCCESS ((AK_STATUS)0x00000000L)
#define AK_STATUS_UNSUCCESSFUL ((AK_STATUS)0xC0000001L)
#define AK_STATUS_TIMEOUT ((AK_STATUS)0x00000102L)
#define AK_STATUS_NO_MORE_ENTRIES ((AK_STATUS)0x8000001AL)
#define AK_STATUS_INVALID_PARAMETER ((AK_STATUS)0xC000000DL)
#define AK_STATUS_NOT_FOUND ((AK_STATUS)0xC0000225L)
#define AK_STATUS_NOT_SUPPORTED ((AK_STATUS)0xC00000BBL)
#define AK_STATUS_INSUFFICIENT_RESOURCES ((AK_STATUS)0xC000009AL)
#define AK_STATUS_DEVICE_NOT_READY ((AK_STATUS)0xC00000A3L)
#define AK_STATUS_ALREADY_EXISTS ((AK_STATUS)0xC0000035L)
#define AK_STATUS_CANCELLED ((AK_STATUS)0xC0000120L)

typedef enum AK_LIFECYCLE_STATE {
    AkStateInit = 0,
    AkStateStarting = 1,
    AkStateRunning = 2,
    AkStateRemoving = 3,
    AkStateClosing = 4,
    AkStateClosed = 5,
    AkStateBroken = 6
} AK_LIFECYCLE_STATE;

typedef enum AK_EVENT_TYPE {
    AkEventDiskOnline = 0,
    AkEventDiskRemoved = 1,
    AkEventWriteFinalCommitted = 2,
    AkEventWriteFinalRejected = 3,
    AkEventSessionBroken = 4
} AK_EVENT_TYPE;

typedef VOID(AK_CALL* AK_LOG_FN)(
    void* log_ctx,
    INT level,
    const char* text);

typedef struct AK_OPEN_PARAMS {
    UINT32 HeartbeatIntervalMs;
    UINT32 InitialEventQueueCapacity;
    AK_LOG_FN LogFn;
    void* LogCtx;
} AK_OPEN_PARAMS;

typedef struct AK_DISK_PARAMS {
    UINT32 TargetId;
    UINT32 SectorSize;
    UINT64 DiskSizeBytes;
    UINT32 QueueDepth;
    UINT32 WriteSlotBytes;
    UINT16 ReadWorkerCount;
    UINT16 WriteWorkerCount;
    UINT32 AckBatchMaxRanges;
} AK_DISK_PARAMS;

typedef struct AK_READ_OP {
    UINT32 TargetId;
    UINT64 DiskRuntimeId;
    UINT64 EventId;
    UINT64 Lba;
    UINT64 OffsetBytes;
    UINT32 BlockCount;
    UINT32 DataLength;
    UINT32 Flags;
} AK_READ_OP;

typedef struct AK_WRITE_OP {
    UINT32 TargetId;
    UINT64 DiskRuntimeId;
    UINT64 EventId;
    UINT32 Seq;
    UINT32 TotalSeq;
    UINT64 Lba;
    UINT64 OffsetBytes;
    UINT32 ByteOffsetInWrite;
    UINT32 DataLength;
    UINT32 Flags;
} AK_WRITE_OP;

typedef struct AK_MEDIA_OPS {
    AK_STATUS(AK_CALL* read_bytes)(
        void* media_ctx,
        const AK_READ_OP* op,
        void* out_buffer,
        UINT32* out_data_length);

    AK_STATUS(AK_CALL* stage_write)(
        void* media_ctx,
        const AK_WRITE_OP* op,
        const void* data_buffer,
        UINT32 data_length);
} AK_MEDIA_OPS;

typedef struct AK_EVENT {
    AK_EVENT_TYPE Type;
    UINT32 TargetId;
    UINT64 DiskRuntimeId;
    UINT64 EventId;
    UINT32 TotalSeq;
    UINT32 Flags;
    AK_STATUS Status;
} AK_EVENT;

typedef struct AK_SESSION_STATE {
    AK_LIFECYCLE_STATE Lifecycle;
    UINT64 SessionId;
    BOOLEAN HeartbeatRunning;
    BOOLEAN TransportReady;
    UINT32 DiskCount;
    AK_STATUS LastError;
} AK_SESSION_STATE;

typedef struct AK_DISK_STATE {
    AK_LIFECYCLE_STATE Lifecycle;
    UINT32 TargetId;
    UINT64 DiskRuntimeId;
    BOOLEAN ReadWorkersRunning;
    BOOLEAN WriteWorkersRunning;
    BOOLEAN AckFlusherRunning;
    AK_STATUS LastError;
} AK_DISK_STATE;

typedef struct AK_SESSION_STATS {
    UINT64 HeartbeatSent;
    UINT64 CommandFailures;
    UINT64 ProtocolFailures;
    UINT64 EventsQueued;
    UINT64 EventsDropped;
} AK_SESSION_STATS;

typedef struct AK_DISK_STATS {
    UINT64 ReadSlotPosts;
    UINT64 ReadSlotCompletions;
    UINT64 ReadAckCommands;
    UINT64 WriteSlotPosts;
    UINT64 WriteSlotCompletions;
    UINT64 WriteAckFlushes;
    UINT64 WriteAckRanges;
    UINT64 WriteAckRangeFailures;
    UINT64 FinalWriteCommitted;
    UINT64 FinalWriteRejected;
} AK_DISK_STATS;

AK_API AK_STATUS AK_CALL AkOpen(
    const AK_OPEN_PARAMS* params,
    AK_SESSION** out_session);

AK_API VOID AK_CALL AkClose(
    AK_SESSION* session);

AK_API AK_STATUS AK_CALL AkRemoveAllDisks(
    AK_SESSION* session);

AK_API AK_STATUS AK_CALL AkQuerySessionState(
    AK_SESSION* session,
    AK_SESSION_STATE* out_state);

AK_API AK_STATUS AK_CALL AkQuerySessionStats(
    AK_SESSION* session,
    AK_SESSION_STATS* out_stats);

AK_API AK_STATUS AK_CALL AkWaitEvent(
    AK_SESSION* session,
    DWORD timeout_ms,
    AK_EVENT* out_event);

AK_API AK_STATUS AK_CALL AkPollEvent(
    AK_SESSION* session,
    AK_EVENT* out_event);

AK_API AK_STATUS AK_CALL AkCreateDisk(
    AK_SESSION* session,
    const AK_DISK_PARAMS* params,
    const AK_MEDIA_OPS* media_ops,
    void* media_ctx,
    AK_DISK** out_disk);

AK_API AK_STATUS AK_CALL AkRemoveDisk(
    AK_DISK* disk);

AK_API AK_STATUS AK_CALL AkQueryDiskState(
    AK_DISK* disk,
    AK_DISK_STATE* out_state);

AK_API AK_STATUS AK_CALL AkQueryDiskStats(
    AK_DISK* disk,
    AK_DISK_STATS* out_stats);

#ifdef __cplusplus
}
#endif
