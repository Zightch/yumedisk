#pragma once

#include <ntddk.h>
#include <wdf.h>

#include "..\..\..\shared\yumedisk_proto.h"

#define DRIVER_NAME "YumeDiskKMDF"
#define MEM_TAG 'KDmY'

typedef struct _CTRL_DEVICE_CONTEXT {
    WDFSPINLOCK OpenLock;
    LONG OpenCount;
    WDFFILEOBJECT OpenFileObject;
    UINT64 SessionId;
    volatile HANDLE MiniportHandle;
} CTRL_DEVICE_CONTEXT, *PCTRL_DEVICE_CONTEXT;

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(CTRL_DEVICE_CONTEXT, ControlGetContext);

