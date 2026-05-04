#pragma once

#include "..\core\defs.h"

NTSTATUS
ControlProxyCommand(
    _Inout_ PCTRL_DEVICE_CONTEXT Context,
    _Inout_updates_bytes_(BufferCapacity) PUCHAR Buffer,
    _In_ ULONG InputLength,
    _In_ ULONG BufferCapacity,
    _Out_ ULONG* BytesReturned
);

VOID
ControlCloseMiniportHandle(
    _Inout_ PCTRL_DEVICE_CONTEXT Context
);

VOID
ControlSendSessionCleanup(
    _Inout_ PCTRL_DEVICE_CONTEXT Context,
    _In_ UINT64 SessionId
);

