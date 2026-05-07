#pragma once

#include "common/ak_internal.h"

AK_STATUS AkEventWait(
    AK_SESSION* session,
    DWORD timeout_ms,
    AK_EVENT* out_event);

AK_STATUS AkEventPoll(
    AK_SESSION* session,
    AK_EVENT* out_event);

AK_STATUS AkEventQueueInitialize(
    AK_SESSION* session);

void AkEventQueueDestroy(
    AK_SESSION* session);

AK_STATUS AkEventQueuePush(
    AK_SESSION* session,
    const AK_EVENT* event_record);

AK_STATUS AkEventQueuePushSessionBroken(
    AK_SESSION* session,
    AK_STATUS status);
