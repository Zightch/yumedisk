#pragma once

#include "..\core\defs.h"

NTSTATUS
ControlTransportRuntimeStart(
    _Inout_ PCTRL_FILE_CONTEXT Context,
    _In_ WDFFILEOBJECT FileObject
);

VOID
ControlTransportRuntimeBeginClose(
    _Inout_ PCTRL_FILE_CONTEXT Context
);

VOID
ControlTransportRuntimeStop(
    _Inout_ PCTRL_FILE_CONTEXT Context
);
