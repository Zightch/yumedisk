#pragma once

#include <ntddk.h>
#include <storport.h>
#include <ntddscsi.h>

#include "..\..\..\shared\yumedisk_proto.h"

#define DRIVER_NAME "YumeDiskSCSI"
#define MEM_TAG 'SDmY'

typedef struct _REPORT_LUNS_DATA {
    UINT32 LunListLength;
    UINT32 Reserved;
    UINT64 Entry[1];
} REPORT_LUNS_DATA, *PREPORT_LUNS_DATA;

typedef struct _YUME_DISK_QUEUE_STATE {
    KSPIN_LOCK ReadQueueLock;
    KSPIN_LOCK WriteQueueLock;
    LIST_ENTRY PostedReadSlots;
    LIST_ENTRY PendingReads;
    LIST_ENTRY PostedWriteSlots;
    LIST_ENTRY PendingWrites;
} YUME_DISK_QUEUE_STATE, *PYUME_DISK_QUEUE_STATE;

typedef struct _YUME_DISK {
    PUCHAR Buffer;
    UINT64 Size;
    KSPIN_LOCK BufferLock;
    YUME_DISK_QUEUE_STATE Queue;
    UINT64 SectorCount;
    UINT32 SectorSize;
    ULONG Generation;
    BOOLEAN Configured;
    BOOLEAN Present;
    BOOLEAN Removing;
    BOOLEAN Reserved;
} YUME_DISK, *PYUME_DISK;

typedef struct _DEVICE_CONTEXT {
    KSPIN_LOCK SessionLock;
    ULONG MaxTargets;
    UINT64 CurrentSessionId;
    volatile LONG64 NextEventId;
    volatile LONG64 DebugProgressCounter;
    volatile LONG64 DebugReadRequestsQueued;
    volatile LONG64 DebugReadSlotsIssued;
    volatile LONG64 DebugReadAcksApplied;
    volatile LONG64 DebugReadRequestsCompleted;
    volatile LONG64 DebugReadRequestsFailed;
    volatile LONG64 DebugWriteRequestsQueued;
    volatile LONG64 DebugWriteFragmentsIssued;
    volatile LONG64 DebugWriteAcksApplied;
    volatile LONG64 DebugWriteRequestsCompleted;
    volatile LONG64 DebugWriteRequestsFailed;
    YUME_DISK Disk[YUMEDISK_MAX_TARGETS];
} DEVICE_CONTEXT, *PDEVICE_CONTEXT;
