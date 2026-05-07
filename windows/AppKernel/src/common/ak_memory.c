#include "common/ak_internal.h"

#include <stdlib.h>
#include <string.h>

void* AkAllocZero(size_t size)
{
    void* memory;

    if (size == 0u) {
        return NULL;
    }

    memory = malloc(size);
    if (memory == NULL) {
        return NULL;
    }

    memset(memory, 0, size);
    return memory;
}

void AkFree(void* ptr)
{
    free(ptr);
}
