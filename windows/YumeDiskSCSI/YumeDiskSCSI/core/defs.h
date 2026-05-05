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

typedef struct _YUME_DISK {
    PUCHAR Buffer;
    UINT64 Size;
    KSPIN_LOCK BufferLock;
    UINT64 SectorCount;
    UINT32 SectorSize;
    ULONG Generation;
    BOOLEAN Configured;
    BOOLEAN Present;
    BOOLEAN Removing;
    BOOLEAN Reserved;
} YUME_DISK, *PYUME_DISK;

typedef struct _YUMEDISK_EVENT_NODE {
    LIST_ENTRY Link;
    YUMEDISK_EVENT Event;
    struct _YUMEDISK_PENDING_IO_NODE* PendingIo;
} YUMEDISK_EVENT_NODE, *PYUMEDISK_EVENT_NODE;

typedef struct _YUMEDISK_WAITER_NODE {
    LIST_ENTRY Link;
    PSTORAGE_REQUEST_BLOCK Srb;
} YUMEDISK_WAITER_NODE, *PYUMEDISK_WAITER_NODE;

typedef enum _YUMEDISK_PENDING_IO_TYPE {
    DiskPendingIoInvalid = 0,
    DiskPendingIoRead = 1,
    DiskPendingIoWrite = 2
} YUMEDISK_PENDING_IO_TYPE;

typedef struct _YUMEDISK_PENDING_IO_NODE {
    LIST_ENTRY Link;
    PSTORAGE_REQUEST_BLOCK Srb;
    ULONG Type;
    ULONG TargetId;
    ULONGLONG TxId;
    ULONGLONG Lba;
    ULONG BlockCount;
    ULONG DataLength;
} YUMEDISK_PENDING_IO_NODE, *PYUMEDISK_PENDING_IO_NODE;

typedef struct _DEVICE_CONTEXT {
    KSPIN_LOCK ControlLock;
    LIST_ENTRY PendingEvents;
    LIST_ENTRY PendingWaiters;
    LIST_ENTRY PendingIo;
    ULONG PendingEventCount;
    ULONG PendingWaiterCount;
    ULONG PendingIoCount;
    ULONG MaxTargets;
    UINT64 CurrentSessionId;
    UINT64 NextTxId;
    YUME_DISK Disk[YUMEDISK_MAX_TARGETS];
} DEVICE_CONTEXT, *PDEVICE_CONTEXT;
