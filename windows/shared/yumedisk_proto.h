#pragma once

#define YUMEDISK_PROTOCOL_VERSION 1u
#define YUMEDISK_MAX_TARGETS 255u
#define YUMEDISK_MIN_TARGET_ID 0u
#define YUMEDISK_MAX_USABLE_TARGET_ID 254u
#define YUMEDISK_USABLE_TARGET_COUNT \
    (YUMEDISK_MAX_USABLE_TARGET_ID - YUMEDISK_MIN_TARGET_ID + 1u)
#define YUMEDISK_DEFAULT_SECTOR_SIZE 512u
#define YUMEDISK_MINIPORT_CONTROL_CODE 0x8001u
#define YUMEDISK_MINIPORT_TIMEOUT_SEC 30u
#define YUMEDISK_SESSION_CLOSE_FLAG 0x00000001u

#define IOCTL_YUMEDISK_APP_COMMAND \
    CTL_CODE(FILE_DEVICE_UNKNOWN, 0x900, METHOD_BUFFERED, FILE_ANY_ACCESS)

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
    YumeDiskCommandWaitEvent = 5,
    YumeDiskCommandHeartbeat = 6,
    YumeDiskCommandReadReply = 7,
    YumeDiskCommandWriteAck = 8
} YUMEDISK_COMMAND;

typedef enum _YUMEDISK_EVENT_TYPE {
    YumeDiskEventNone = 0,
    YumeDiskEventDiskAdded = 1,
    YumeDiskEventDiskRemoved = 2,
    YumeDiskEventShutdown = 3,
    YumeDiskEventReadRequest = 4,
    YumeDiskEventWriteRequest = 5
} YUMEDISK_EVENT_TYPE;

typedef enum _YUMEDISK_EVENT_FLAGS {
    YumeDiskEventFlagHasInlineData = 0x00000001u
} YUMEDISK_EVENT_FLAGS;

typedef enum _YUMEDISK_FEATURE_FLAGS {
    YumeDiskFeatureWaitEvent = 0x00000001u,
    YumeDiskFeatureDynamicDisk = 0x00000002u,
    YumeDiskFeatureIoSkeleton = 0x00000004u
} YUMEDISK_FEATURE_FLAGS;

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

typedef struct _YUMEDISK_WAIT_EVENT {
    ULONG TimeoutMs;
    ULONG Reserved;
} YUMEDISK_WAIT_EVENT, *PYUMEDISK_WAIT_EVENT;

typedef struct _YUMEDISK_EVENT {
    ULONG EventType;
    ULONG EventFlags;
    ULONG TargetId;
    ULONG Reserved;
    ULONGLONG TxId;
    ULONGLONG Lba;
    ULONG BlockCount;
    ULONG DataLength;
} YUMEDISK_EVENT, *PYUMEDISK_EVENT;

typedef struct _YUMEDISK_READ_REPLY {
    ULONGLONG TxId;
    LONG IoStatus;
    ULONG DataLength;
    ULONG Reserved;
    UCHAR Data[1];
} YUMEDISK_READ_REPLY, *PYUMEDISK_READ_REPLY;

#define YUMEDISK_READ_REPLY_BASE_SIZE FIELD_OFFSET(YUMEDISK_READ_REPLY, Data)

typedef struct _YUMEDISK_WRITE_ACK {
    ULONGLONG TxId;
    LONG IoStatus;
    ULONG Reserved0;
    ULONG Reserved1;
} YUMEDISK_WRITE_ACK, *PYUMEDISK_WRITE_ACK;
