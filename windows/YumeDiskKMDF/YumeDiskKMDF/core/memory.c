#include "memory.h"

PVOID
ControlAlloc(
    _In_ SIZE_T Size
)
{
    return ExAllocatePool2(POOL_FLAG_NON_PAGED, Size, MEM_TAG);
}

VOID
ControlFree(
    _In_opt_ PVOID Pointer
)
{
    if (Pointer != NULL) {
        ExFreePoolWithTag(Pointer, MEM_TAG);
    }
}

