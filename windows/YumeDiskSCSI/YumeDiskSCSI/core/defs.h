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

typedef struct _DEVICE_CONTEXT {
    KSPIN_LOCK ControlLock;
    ULONG MaxTargets;
    UINT64 CurrentSessionId;
    YUME_DISK Disk[YUMEDISK_MAX_TARGETS];
} DEVICE_CONTEXT, *PDEVICE_CONTEXT;
