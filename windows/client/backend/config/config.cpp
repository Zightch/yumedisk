#include "backend/config/config.h"

namespace clientbackend {

std::wstring mediaModeToText(MediaMode mode) {
    switch (mode) {
    case MediaMode::autoSelect:
        return L"auto";
    case MediaMode::denseMem:
        return L"denseMem";
    case MediaMode::sparseMem:
        return L"sparseMem";
    case MediaMode::rawFile:
        return L"rawFile";
    default:
        return L"unknown";
    }
}

MediaMode resolveMediaMode(MediaMode requestedMode, uint64_t diskSizeBytes) {
    if (requestedMode == MediaMode::autoSelect) {
        return diskSizeBytes <= maxDenseMediaBytes
            ? MediaMode::denseMem
            : MediaMode::sparseMem;
    }

    return requestedMode;
}

} // namespace clientbackend
