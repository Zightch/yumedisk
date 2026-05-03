#include "utils.h"

#include "define.h"

void* malloc(size_t size) {
    return ExAllocatePool2(POOL_FLAG_NON_PAGED, size, MEM_TAG);
}

void free(void* p) {
    ExFreePoolWithTag(p, MEM_TAG);
}
