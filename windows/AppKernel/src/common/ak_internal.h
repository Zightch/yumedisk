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
} AK_DISK_WORKER_CONTEXT;

struct AK_SESSION {
    AK_OPEN_PARAMS OpenParams;
    AK_SESSION_STATE State;
    AK_SESSION_STATS Stats;
    AK_EVENT_QUEUE EventQueue;
    SRWLOCK Lock;
    HANDLE ControlFile;
    HANDLE StopEvent;
    HANDLE HeartbeatThread;
    YUMEDISK_QUERY_INFO QueryInfo;
    struct AK_DISK* DiskListHead;
    UINT64 NextDiskRuntimeId;
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
    UINT32 PrimedReadSlotDepth;
    BOOLEAN RegisteredInSession;
    struct AK_DISK* SessionNext;
};

void* AkAllocZero(size_t size);
void AkFree(void* ptr);
AK_STATUS AkFromWin32Error(DWORD error);
