#pragma once

#include <ntddk.h>
#include <wdf.h>

#include "..\..\..\shared\yumedisk_proto.h"

#define DRIVER_NAME "YumeDiskKMDF"
#define MEM_TAG 'KDmY'
#define YUMEDISK_TRANSPORT_SLOT_COUNT 8

#define YD_KMDF_LOG(_fmt_, ...) \
    DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_INFO_LEVEL, DRIVER_NAME ": " _fmt_ "\n", __VA_ARGS__)

#define YD_KMDF_ERR(_fmt_, ...) \
    DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL, DRIVER_NAME ": " _fmt_ "\n", __VA_ARGS__)

typedef struct _CTRL_TRANSPORT_SLOT {
    FAST_MUTEX Lock;
    HANDLE Handle;
    PUCHAR IoctlBuffer;
    ULONG IoctlBufferCapacity;
} CTRL_TRANSPORT_SLOT, *PCTRL_TRANSPORT_SLOT;

typedef struct _CTRL_DEVICE_CONTEXT {
    WDFSPINLOCK OpenLock;
    LONG OpenCount;
    WDFFILEOBJECT OpenFileObject;
    UINT64 SessionId;
    FAST_MUTEX TransportStateLock;
    LONG TransportNextSlot;
    ULONG TransportOpenSlotCount;
    BOOLEAN TransportOnline;
    CTRL_TRANSPORT_SLOT TransportSlots[YUMEDISK_TRANSPORT_SLOT_COUNT];
} CTRL_DEVICE_CONTEXT, *PCTRL_DEVICE_CONTEXT;

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(CTRL_DEVICE_CONTEXT, ControlGetContext);

