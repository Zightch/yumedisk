#pragma once

#include "common/ak_internal.h"

AK_STATUS AkProtocolOpenControlDevice(
    HANDLE* out_file);

AK_STATUS AkProtocolQueryInfo(
    HANDLE file,
    YUMEDISK_QUERY_INFO* out_info,
    UINT64* out_session_id);

AK_STATUS AkProtocolQuerySessionId(
    HANDLE file,
    UINT64* out_session_id);

AK_STATUS AkProtocolSendHeartbeat(
    HANDLE file,
    UINT64 session_id);

AK_STATUS AkProtocolRemoveAllDisks(
    HANDLE file,
    UINT64 session_id,
    ULONG flags);

AK_STATUS AkProtocolUnavailable(void);
