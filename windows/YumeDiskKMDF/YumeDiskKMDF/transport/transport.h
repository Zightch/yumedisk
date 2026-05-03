#pragma once

#include "..\core\defs.h"

VOID
ControlTransportInitialize(
    _Out_ PCTRL_DEVICE_CONTEXT Context
);

NTSTATUS
ControlTransportOpenSession(
    _Inout_ PCTRL_DEVICE_CONTEXT Context
);

VOID
ControlTransportCloseSession(
    _Inout_ PCTRL_DEVICE_CONTEXT Context
);

BOOLEAN
ControlTransportIsOnline(
    _In_ PCTRL_DEVICE_CONTEXT Context
);

NTSTATUS
ControlProxyCommand(
    _In_ PCTRL_DEVICE_CONTEXT Context,
    _Inout_updates_bytes_(BufferCapacity) PUCHAR Buffer,
    _In_ ULONG InputLength,
    _In_ ULONG BufferCapacity,
    _Out_ ULONG* BytesReturned
);

VOID
ControlSendSessionCleanup(
    _In_ PCTRL_DEVICE_CONTEXT Context,
    _In_ UINT64 SessionId
);

