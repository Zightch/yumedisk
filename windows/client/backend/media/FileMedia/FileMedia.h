#pragma once

#include <Windows.h>

#include <cstdint>
#include <mutex>

#include "backend/media/Media/Media.h"

namespace clientbackend {

class FileMedia : public Media {
public:
    ~FileMedia() override;

    bool readLocked(
        UINT64 offset,
        void* buffer,
        UINT32 length) override;

    bool writeLocked(
        UINT64 offset,
        const void* buffer,
        UINT32 length) override;

    uint64_t sizeBytes() const override;

protected:
    FileMedia(
        HANDLE backingFile,
        uint64_t mediaSizeBytes);

private:
    bool seekLocked(UINT64 offset);

    HANDLE backingFile = INVALID_HANDLE_VALUE;
    uint64_t mediaSizeBytes = 0;
    std::mutex backingFileIoLock;
};

} // namespace clientbackend
