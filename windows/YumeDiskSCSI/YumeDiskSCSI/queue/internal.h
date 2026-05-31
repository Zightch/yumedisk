#pragma once

#include "..\core\protocol.h"

/*
 * Queue-private helpers shared by queue subcomponents.
 * This header must stay inside queue/ and is not a public facade.
 */

static __forceinline
UINT64
DiskNextEventId(
    _Inout_ PDEVICE_CONTEXT Extension
)
{
    return (UINT64)InterlockedIncrement64(&Extension->NextEventId);
}

static __forceinline
VOID
DiskTickProgress(
    _Inout_ PDEVICE_CONTEXT Extension
)
{
    InterlockedIncrement64(&Extension->DebugProgressCounter);
}
