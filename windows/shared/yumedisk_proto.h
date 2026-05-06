#pragma once

#define YUMEDISK_PROTOCOL_VERSION 1u
#define YUMEDISK_MAX_TARGETS 255u
#define YUMEDISK_MIN_TARGET_ID 0u
#define YUMEDISK_MAX_USABLE_TARGET_ID 254u
#define YUMEDISK_USABLE_TARGET_COUNT \
    (YUMEDISK_MAX_USABLE_TARGET_ID - YUMEDISK_MIN_TARGET_ID + 1u)
#define YUMEDISK_DEFAULT_SECTOR_SIZE 512u
#define YUMEDISK_MINIPORT_CONTROL_CODE 0x8001u
#define YUMEDISK_MINIPORT_TIMEOUT_SEC 3600u
#define YUMEDISK_SESSION_CLOSE_FLAG 0x00000001u

#define IOCTL_YUMEDISK_APP_COMMAND \
    CTL_CODE(FILE_DEVICE_UNKNOWN, 0x900, METHOD_OUT_DIRECT, FILE_ANY_ACCESS)

#define YUMEDISK_MINIPORT_SIGNATURE "YMDISK1"

static const GUID GUID_YUMEDISK_CONTROL = {
    0x72d587ef, 0xab50, 0x490a, { 0x9f, 0xf2, 0x90, 0x72, 0xcd, 0xe5, 0x1d, 0x42 }
};

typedef enum _YUMEDISK_COMMAND {
    YumeDiskCommandInvalid = 0,
    YumeDiskCommandQueryInfo = 1,
    YumeDiskCommandCreateDisk = 2,
    YumeDiskCommandRemoveDisk = 3,
    YumeDiskCommandRemoveAllDisks = 4,
    YumeDiskCommandHeartbeat = 5,
    YumeDiskCommandPostReadSlot = 20,
    YumeDiskCommandPostWriteSlot = 21,
    YumeDiskCommandReadAck = 22,
    YumeDiskCommandWriteAckBatch = 23,
    YumeDiskCommandSubmitSlot = 24,
    YumeDiskCommandCancelSlot = 25,
    YumeDiskCommandQueryDebugState = 26
} YUMEDISK_COMMAND;

typedef enum _YUMEDISK_FEATURE_FLAGS {
    YumeDiskFeatureDynamicDisk = 0x00000002u,
    YumeDiskFeatureAppOwnedQueue = 0x00000008u
} YUMEDISK_FEATURE_FLAGS;

typedef enum _YUMEDISK_SLOT_TYPE {
    YumeDiskSlotTypeInvalid = 0,
    YumeDiskSlotTypeRead = 1,
    YumeDiskSlotTypeWrite = 2
} YUMEDISK_SLOT_TYPE;

typedef enum _YUMEDISK_SLOT_FLAGS {
    YumeDiskSlotFlagNone = 0x00000000u
} YUMEDISK_SLOT_FLAGS;

typedef struct _YUMEDISK_HEADER {
    ULONG Size;
    ULONG Version;
    ULONG Command;
    LONG Status;
    ULONGLONG SessionId;
    ULONGLONG TxId;
    ULONG TargetId;
    ULONG Flags;
    ULONG PayloadLength;
    ULONG Reserved;
} YUMEDISK_HEADER, *PYUMEDISK_HEADER;

typedef struct _YUMEDISK_MESSAGE {
    YUMEDISK_HEADER Header;
    UCHAR Payload[1];
} YUMEDISK_MESSAGE, *PYUMEDISK_MESSAGE;

#define YUMEDISK_MESSAGE_BASE_SIZE FIELD_OFFSET(YUMEDISK_MESSAGE, Payload)

typedef struct _YUMEDISK_QUERY_INFO {
    ULONG ProtocolVersion;
    ULONG MaxTargets;
    ULONG Features;
    ULONG Reserved;
    CHAR AdapterSignature[8];
    WCHAR ServiceName[16];
} YUMEDISK_QUERY_INFO, *PYUMEDISK_QUERY_INFO;

typedef struct _YUMEDISK_CREATE_DISK {
    ULONG TargetId;
    ULONG SectorSize;
    ULONGLONG SectorCount;
    ULONGLONG DiskSizeBytes;
} YUMEDISK_CREATE_DISK, *PYUMEDISK_CREATE_DISK;

typedef struct _YUMEDISK_REMOVE_DISK {
    ULONG TargetId;
    ULONG Flags;
} YUMEDISK_REMOVE_DISK, *PYUMEDISK_REMOVE_DISK;

typedef struct _YUMEDISK_SLOT_DESCRIPTOR {
    UINT64 SessionId;
    UINT64 SlotId;
    UINT32 SlotType;
    UINT32 TargetId;
    UINT64 KernelVa;
    UINT32 Capacity;
    UINT32 Flags;
} YUMEDISK_SLOT_DESCRIPTOR, *PYUMEDISK_SLOT_DESCRIPTOR;

typedef struct _YUMEDISK_SUBMIT_SLOT {
    YUMEDISK_SLOT_DESCRIPTOR Slot;
} YUMEDISK_SUBMIT_SLOT, *PYUMEDISK_SUBMIT_SLOT;

#define YUMEDISK_SUBMIT_SLOT_BASE_SIZE sizeof(YUMEDISK_SUBMIT_SLOT)
#define YUMEDISK_SUBMIT_SLOT_SIZE() YUMEDISK_SUBMIT_SLOT_BASE_SIZE

typedef struct _YUMEDISK_CANCEL_SLOT {
    UINT64 SlotId;
    UINT32 SlotType;
    UINT32 TargetId;
} YUMEDISK_CANCEL_SLOT, *PYUMEDISK_CANCEL_SLOT;

typedef struct _YUMEDISK_READ_SLOT_EVENT {
    UINT64 EventId;
    UINT32 TargetId;
    UINT32 Reserved0;
    UINT64 Lba;
    UINT32 BlockCount;
    UINT32 DataLength;
} YUMEDISK_READ_SLOT_EVENT, *PYUMEDISK_READ_SLOT_EVENT;

typedef struct _YUMEDISK_READ_ACK {
    UINT64 EventId;
    LONG IoStatus;
    UINT32 DataLength;
    UINT64 KernelVa;
} YUMEDISK_READ_ACK, *PYUMEDISK_READ_ACK;

typedef struct _YUMEDISK_WRITE_SLOT_HEADER {
    UINT64 EventId;
    UINT32 Seq;
    UINT32 TotalSeq;
    UINT32 TargetId;
    UINT32 Reserved0;
    UINT64 Lba;
    UINT32 ByteOffsetInWrite;
    UINT32 DataLength;
    UINT32 Flags;
    UINT32 Reserved1;
    UCHAR Data[1];
} YUMEDISK_WRITE_SLOT_HEADER, *PYUMEDISK_WRITE_SLOT_HEADER;

#define YUMEDISK_WRITE_SLOT_HEADER_BASE_SIZE FIELD_OFFSET(YUMEDISK_WRITE_SLOT_HEADER, Data)

typedef struct _YUMEDISK_WRITE_ACK_RANGE {
    UINT64 EventId;
    UINT32 SeqBase;
    UINT32 SeqCount;
    LONG IoStatus;
    UINT32 Reserved;
} YUMEDISK_WRITE_ACK_RANGE, *PYUMEDISK_WRITE_ACK_RANGE;

typedef struct _YUMEDISK_WRITE_ACK_BATCH {
    UINT32 RangeCount;
    UINT32 Reserved;
    YUMEDISK_WRITE_ACK_RANGE Ranges[1];
} YUMEDISK_WRITE_ACK_BATCH, *PYUMEDISK_WRITE_ACK_BATCH;

#define YUMEDISK_WRITE_ACK_BATCH_BASE_SIZE FIELD_OFFSET(YUMEDISK_WRITE_ACK_BATCH, Ranges)
#define YUMEDISK_WRITE_ACK_BATCH_SIZE(_rangeCount) \
    (YUMEDISK_WRITE_ACK_BATCH_BASE_SIZE + ((_rangeCount) * sizeof(YUMEDISK_WRITE_ACK_RANGE)))

typedef struct _YUMEDISK_WRITE_ACK_FAILURE {
    UINT32 RangeIndex;
    LONG Status;
} YUMEDISK_WRITE_ACK_FAILURE, *PYUMEDISK_WRITE_ACK_FAILURE;

typedef struct _YUMEDISK_WRITE_ACK_BATCH_RESULT {
    UINT32 FailureCount;
    UINT32 Reserved;
    YUMEDISK_WRITE_ACK_FAILURE Failures[1];
} YUMEDISK_WRITE_ACK_BATCH_RESULT, *PYUMEDISK_WRITE_ACK_BATCH_RESULT;

#define YUMEDISK_WRITE_ACK_BATCH_RESULT_BASE_SIZE FIELD_OFFSET(YUMEDISK_WRITE_ACK_BATCH_RESULT, Failures)
#define YUMEDISK_WRITE_ACK_BATCH_RESULT_SIZE(_failureCount) \
    (YUMEDISK_WRITE_ACK_BATCH_RESULT_BASE_SIZE + ((_failureCount) * sizeof(YUMEDISK_WRITE_ACK_FAILURE)))

typedef struct _YUMEDISK_DEBUG_STATE {
    UINT64 ActiveSessionId;
    UINT64 ProgressCounter;
    UINT64 ReadRequestsQueued;
    UINT64 ReadSlotsIssued;
    UINT64 ReadAcksApplied;
    UINT64 ReadRequestsCompleted;
    UINT64 ReadRequestsFailed;
    UINT64 WriteRequestsQueued;
    UINT64 WriteFragmentsIssued;
    UINT64 WriteAcksApplied;
    UINT64 WriteRequestsCompleted;
    UINT64 WriteRequestsFailed;
    UINT32 PostedReadSlots;
    UINT32 PendingReads;
    UINT32 PendingReadsIssued;
    UINT32 PostedWriteSlots;
    UINT32 PendingWrites;
    UINT32 PendingWriteFragmentsIssued;
    UINT32 PendingWriteFragmentsAcked;
    UINT32 Reserved;
} YUMEDISK_DEBUG_STATE, *PYUMEDISK_DEBUG_STATE;
