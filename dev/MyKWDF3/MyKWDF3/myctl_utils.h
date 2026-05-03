#pragma once

#include <ntddk.h>

PVOID MyCtlAlloc(
    _In_ SIZE_T Size
);

VOID MyCtlFree(
    _In_opt_ PVOID Pointer
);
