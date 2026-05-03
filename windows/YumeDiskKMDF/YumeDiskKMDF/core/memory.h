#pragma once

#include "defs.h"

PVOID
ControlAlloc(
    _In_ SIZE_T Size
);

VOID
ControlFree(
    _In_opt_ PVOID Pointer
);

