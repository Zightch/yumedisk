#pragma once

#include <windows.h>
#include <winioctl.h>
#include <stddef.h>

#include "appkernel.h"
#include "yumedisk_proto.h"

typedef struct AK_EVENT_QUEUE {
    UINT32 InitialCapacity;
    AK_EVENT* Items;
    UINT32 Capacity;
    UINT32 Head;
    UINT32 Count;
    BOOLEAN SessionBrokenQueued;
    HANDLE WaitEvent;
} AK_EVENT_QUEUE;

typedef struct AK_DISK_WORKER_CONTEXT {
    struct AK_DISK* Disk;
    UINT32 WorkerIndex;
    UINT32 WorkerKind;
    UINT32 SlotCount;
} AK_DISK_WORKER_CONTEXT;

typedef struct AK_WRITE_ACK_NODE AK_WRITE_ACK_NODE;
typedef struct AK_WRITE_EVENT_RECORD AK_WRITE_EVENT_RECORD;

struct AK_SESSION {
    AK_OPEN_PARAMS OpenParams;
    AK_SESSION_STATE State;
    AK_SESSION_STATS Stats;
    AK_EVENT_QUEUE EventQueue;
    SRWLOCK Lock;
    HANDLE ControlFile;
    HANDLE StopEvent;
    HANDLE HeartbeatThread;
    YUMEDISK_KMDF_INFO KmdfInfo;
    YUMEDISK_SCSI_INFO ScsiInfo;
    struct AK_DISK* DiskListHead;
    UINT64 NextDiskRuntimeId;
    UINT64 NextTxId;
};

struct AK_DISK {
    struct AK_SESSION* Session;
    AK_DISK_PARAMS Params;
    AK_MEDIA_OPS MediaOps;
    void* MediaCtx;
    AK_DISK_STATE State;
    AK_DISK_STATS Stats;
    SRWLOCK Lock;
    HANDLE StopEvent;
    HANDLE* ReadWorkerThreads;
    HANDLE* WriteWorkerThreads;
    AK_DISK_WORKER_CONTEXT* ReadWorkerContexts;
    AK_DISK_WORKER_CONTEXT* WriteWorkerContexts;
    HANDLE AckFlusherThread;
    AK_DISK_WORKER_CONTEXT AckFlusherContext;
    HANDLE WriteAckWakeEvent;
    SRWLOCK WriteAckLock;
    AK_WRITE_ACK_NODE* WriteAckHead;
    AK_WRITE_ACK_NODE* WriteAckTail;
    UINT32 PendingWriteAckCount;
    SRWLOCK WriteTrackLock;
    AK_WRITE_EVENT_RECORD* ActiveWriteEvents;
    AK_WRITE_EVENT_RECORD* FinalizedWriteEventsHead;
    AK_WRITE_EVENT_RECORD* FinalizedWriteEventsTail;
    UINT32 FinalizedWriteEventCount;
    UINT32 PrimedReadSlotDepth;
    BOOLEAN RegisteredInSession;
    struct AK_DISK* SessionNext;
};

void* AkAllocZero(size_t size);
void AkFree(void* ptr);
AK_STATUS AkFromWin32Error(DWORD error);
