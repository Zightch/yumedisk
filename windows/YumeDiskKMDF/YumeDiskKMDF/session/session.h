#pragma once

#include "..\core\defs.h"

VOID
ControlSessionInitialize(
    _Out_ PCTRL_DEVICE_CONTEXT Context
);

NTSTATUS
ControlSessionTryOpen(
    _Inout_ PCTRL_DEVICE_CONTEXT Context,
    _In_ WDFFILEOBJECT FileObject,
    _Out_opt_ UINT64* SessionId
);

VOID
ControlSessionCleanup(
    _Inout_ PCTRL_DEVICE_CONTEXT Context,
    _In_ WDFFILEOBJECT FileObject
);

NTSTATUS
ControlSessionHeartbeat(
    _In_ WDFFILEOBJECT FileObject,
    _Out_opt_ UINT64* SessionId
);

NTSTATUS
ControlSessionAcquire(
    _In_ WDFFILEOBJECT FileObject,
    _Outptr_ PCTRL_FILE_CONTEXT* SessionContext,
    _Out_opt_ UINT64* SessionId
);

VOID
ControlSessionRelease(
    _In_ PCTRL_FILE_CONTEXT SessionContext
);

NTSTATUS
ControlSessionAcquireSlot(
    _In_ WDFFILEOBJECT FileObject,
    _Outptr_ PCTRL_FILE_CONTEXT* SessionContext,
    _Out_ UINT64* SessionId
);

VOID
ControlSessionReleaseSlot(
    _In_ PCTRL_FILE_CONTEXT SessionContext
);
