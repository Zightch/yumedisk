#pragma once

#include <ntddk.h>
#include <wdf.h>

#include "..\..\..\shared\yumedisk_proto.h"

#define DRIVER_NAME "YumeDiskKMDF"
#define MEM_TAG 'KDmY'

typedef enum _CTRL_SESSION_STATE {
    CtrlSessionStateInvalid = 0,
    CtrlSessionStateActive = 1,
    CtrlSessionStateLocked = 2,
    CtrlSessionStateClosing = 3,
    CtrlSessionStateClosed = 4
} CTRL_SESSION_STATE;

typedef struct _CTRL_DEVICE_CONTEXT {
    WDFSPINLOCK OpenLock;
    LONG OpenCount;
    WDFFILEOBJECT OpenFileObject;
} CTRL_DEVICE_CONTEXT, *PCTRL_DEVICE_CONTEXT;

typedef struct _CTRL_FILE_CONTEXT {
    WDFWAITLOCK SessionLock;
    WDFTIMER WatchdogTimer;
    HANDLE MiniportHandle;
    PFILE_OBJECT MiniportFileObject;
    PDEVICE_OBJECT MiniportDeviceObject;
    UINT64 SessionId;
    LONGLONG LastHeartbeatTick;
    KEVENT InFlightZeroEvent;
    volatile LONG InFlightRequestCount;
    KEVENT PendingSlotZeroEvent;
    volatile LONG PendingSlotCount;
    ULONG State;
} CTRL_FILE_CONTEXT, *PCTRL_FILE_CONTEXT;

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(CTRL_DEVICE_CONTEXT, ControlGetContext);
WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(CTRL_FILE_CONTEXT, ControlGetFileContext);
