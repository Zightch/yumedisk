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

UINT64
ControlSessionClose(
    _Inout_ PCTRL_DEVICE_CONTEXT Context,
    _In_ WDFFILEOBJECT FileObject
);

UINT64
ControlSessionGetActiveId(
    _In_ PCTRL_DEVICE_CONTEXT Context
);

