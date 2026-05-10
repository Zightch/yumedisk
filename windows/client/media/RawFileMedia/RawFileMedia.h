#pragma once

#include <memory>
#include <string>

#include "media/FileMedia/FileMedia.h"

namespace BackendCore {

class RawFileMedia final : public FileMedia {
public:
    static std::unique_ptr<RawFileMedia> open(
        const std::wstring& filePath,
        bool readOnly,
        ULONG sectorSize,
        std::wstring* outReason);

private:
    RawFileMedia(
        HANDLE backingFile,
        uint64_t mediaSizeBytes);
};

} // namespace BackendCore

