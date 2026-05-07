#include "event/ak_event.h"

AK_STATUS AkEventWait(
    AK_SESSION* session,
    DWORD timeout_ms,
    AK_EVENT* out_event)
{
    (void)session;
    (void)timeout_ms;
    (void)out_event;
    return AK_STATUS_NOT_SUPPORTED;
}

AK_STATUS AkEventPoll(
    AK_SESSION* session,
    AK_EVENT* out_event)
{
    (void)session;
    (void)out_event;
    return AK_STATUS_NOT_SUPPORTED;
}
