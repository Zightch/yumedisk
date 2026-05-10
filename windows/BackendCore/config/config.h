#pragma once

#include <string>

#include "BackendCore.h"
#include "appkernel.h"

namespace BackendCore {

bool validateSessionConfig(
    const SessionConfig& sessionConfig,
    std::wstring* outErrorText = nullptr);
bool validateDiskConfig(
    const DiskConfig& diskConfig,
    std::wstring* outErrorText = nullptr);
bool validateCreateDiskInputs(
    const DiskConfig& diskConfig,
    const Media* media,
    std::wstring* outErrorText = nullptr);
AK_OPEN_PARAMS buildAkOpenParams(
    const SessionConfig& sessionConfig,
    AK_LOG_FN logFn,
    void* logCtx);
AK_DISK_PARAMS buildAkDiskParams(
    const DiskConfig& diskConfig);

} // namespace BackendCore

