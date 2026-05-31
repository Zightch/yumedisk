#pragma once

#include "common/ak_internal.h"

AK_STATUS AkResponseWait(
    AK_SESSION* session,
    DWORD timeout_ms,
    AK_RESPONSE* out_response);

AK_STATUS AkResponsePoll(
    AK_SESSION* session,
    AK_RESPONSE* out_response);

AK_STATUS AkResponseQueueInitialize(
    AK_SESSION* session);

void AkResponseQueueDestroy(
    AK_SESSION* session);

AK_STATUS AkResponseQueuePush(
    AK_SESSION* session,
    const AK_RESPONSE* response_record);

AK_STATUS AkSessionNoticeWait(
    AK_SESSION* session,
    DWORD timeout_ms,
    AK_SESSION_NOTICE* out_notice);

AK_STATUS AkSessionNoticePoll(
    AK_SESSION* session,
    AK_SESSION_NOTICE* out_notice);

AK_STATUS AkSessionNoticeQueueInitialize(
    AK_SESSION* session);

void AkSessionNoticeQueueDestroy(
    AK_SESSION* session);

AK_STATUS AkSessionNoticeQueuePush(
    AK_SESSION* session,
    const AK_SESSION_NOTICE* notice_record);

AK_STATUS AkSessionNoticeQueuePushBroken(
    AK_SESSION* session,
    AK_STATUS status);
