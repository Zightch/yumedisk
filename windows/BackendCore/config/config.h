#pragma once

#include <string>

#include "types/types.h"

namespace clientbackend {

std::wstring mediaKindToText(MediaKind mediaKind);
bool validateSessionConfig(
    const SessionConfig& sessionConfig,
    std::wstring* outErrorText = nullptr);
bool validateDiskConfig(
    const DiskConfig& diskConfig,
    std::wstring* outErrorText = nullptr);
bool validateCreateDiskRequest(
    const CreateDiskRequest& request,
    std::wstring* outErrorText = nullptr);
AK_OPEN_PARAMS buildAkOpenParams(
    const SessionConfig& sessionConfig,
    AK_LOG_FN logFn,
    void* logCtx);
AK_DISK_PARAMS buildAkDiskParams(
    const DiskConfig& diskConfig);

} // namespace clientbackend
