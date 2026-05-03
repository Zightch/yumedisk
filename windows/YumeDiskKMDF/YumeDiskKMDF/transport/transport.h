#pragma once

#include "..\core\defs.h"

NTSTATUS
ControlProxyCommand(
    _Inout_updates_bytes_(BufferCapacity) PUCHAR Buffer,
    _In_ ULONG InputLength,
    _In_ ULONG BufferCapacity,
    _Out_ ULONG* BytesReturned
);

VOID
ControlSendSessionCleanup(
    _In_ UINT64 SessionId
);

