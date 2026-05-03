#pragma once

#include <ntddk.h>
#include <wdf.h>

#include "..\..\yumedisk\shared\yumedisk_proto.h"

#define DRIVER_NAME "MyKWDF3"
#define MEM_TAG '3KwY'

typedef struct _CTRL_DEVICE_CONTEXT {
    WDFSPINLOCK OpenLock;
    LONG OpenCount;
    WDFFILEOBJECT OpenFileObject;
    UINT64 SessionId;
} CTRL_DEVICE_CONTEXT, *PCTRL_DEVICE_CONTEXT;
