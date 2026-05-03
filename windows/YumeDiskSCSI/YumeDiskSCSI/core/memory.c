#include "memory.h"

PVOID
DiskAlloc(
    _In_ SIZE_T Size
)
{
    return ExAllocatePool2(POOL_FLAG_NON_PAGED, Size, MEM_TAG);
}

VOID
DiskFree(
    _In_opt_ PVOID Pointer
)
{
    if (Pointer != NULL) {
        ExFreePoolWithTag(Pointer, MEM_TAG);
    }
}

