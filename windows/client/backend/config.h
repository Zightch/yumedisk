#pragma once

#include <string>

#include "types.h"

namespace clientbackend {

std::wstring mediaModeToText(MediaMode mode);
MediaMode resolveMediaMode(MediaMode requestedMode, uint64_t diskSizeBytes);

} // namespace clientbackend
