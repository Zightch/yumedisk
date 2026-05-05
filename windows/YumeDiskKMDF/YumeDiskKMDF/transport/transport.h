#pragma once

#include "..\core\defs.h"

NTSTATUS
ControlOpenMiniportHandle(
    _Out_ HANDLE* Handle
);

NTSTATUS
ControlProxyCommand(
    _In_ PCTRL_FILE_CONTEXT Context,
    _Inout_updates_bytes_(BufferCapacity) PUCHAR Buffer,
    _In_ ULONG InputLength,
    _In_ ULONG BufferCapacity,
    _Out_ ULONG* BytesReturned
);

VOID
ControlCloseMiniportHandle(
    _Inout_ PCTRL_FILE_CONTEXT Context
);

VOID
ControlSendSessionCleanup(
    _In_ PCTRL_FILE_CONTEXT Context
);
