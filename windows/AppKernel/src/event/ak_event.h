#pragma once

#include "common/ak_internal.h"

AK_STATUS AkEventWait(
    AK_SESSION* session,
    DWORD timeout_ms,
    AK_EVENT* out_event);

AK_STATUS AkEventPoll(
    AK_SESSION* session,
    AK_EVENT* out_event);
