#pragma once

#include "defs.h"

PVOID
DiskAlloc(
    _In_ SIZE_T Size
);

VOID
DiskFree(
    _In_opt_ PVOID Pointer
);

