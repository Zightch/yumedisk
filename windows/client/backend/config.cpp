#include "config.h"

namespace clientbackend {

std::wstring mediaModeToText(MediaMode mode) {
    switch (mode) {
    case MediaMode::autoSelect:
        return L"auto";
    case MediaMode::dense:
        return L"dense";
    case MediaMode::sparse:
        return L"sparse";
    default:
        return L"unknown";
    }
}

MediaMode resolveMediaMode(MediaMode requestedMode, uint64_t diskSizeBytes) {
    if (requestedMode == MediaMode::autoSelect) {
        return diskSizeBytes <= maxDenseMediaBytes
            ? MediaMode::dense
            : MediaMode::sparse;
    }

    return requestedMode;
}

} // namespace clientbackend
