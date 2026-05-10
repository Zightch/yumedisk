#pragma once

#include <string>

#include "appkernel.h"
#include "types/types.h"

namespace clientbackend {

class Media;

std::wstring mediaKindToText(MediaKind mediaKind);
bool validateSessionConfig(
    const SessionConfig& sessionConfig,
    std::wstring* outErrorText = nullptr);
bool validateDiskConfig(
    const DiskConfig& diskConfig,
    std::wstring* outErrorText = nullptr);
bool validateCreateDiskInputs(
    const DiskConfig& diskConfig,
    MediaKind mediaKind,
    const Media* media,
    std::wstring* outErrorText = nullptr);
AK_OPEN_PARAMS buildAkOpenParams(
    const SessionConfig& sessionConfig,
    AK_LOG_FN logFn,
    void* logCtx);
AK_DISK_PARAMS buildAkDiskParams(
    const DiskConfig& diskConfig);

} // namespace clientbackend
