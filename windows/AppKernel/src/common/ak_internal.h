#pragma once

#include <stddef.h>

#include "appkernel.h"

typedef struct AK_EVENT_QUEUE {
    UINT32 Reserved;
} AK_EVENT_QUEUE;

struct AK_SESSION {
    AK_OPEN_PARAMS OpenParams;
    AK_SESSION_STATE State;
    AK_SESSION_STATS Stats;
    AK_EVENT_QUEUE EventQueue;
};

struct AK_DISK {
    AK_DISK_PARAMS Params;
    AK_MEDIA_OPS MediaOps;
    void* MediaCtx;
    AK_DISK_STATE State;
    AK_DISK_STATS Stats;
};

void* AkAllocZero(size_t size);
void AkFree(void* ptr);
