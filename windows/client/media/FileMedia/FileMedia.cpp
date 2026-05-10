#include "media/FileMedia/FileMedia.h"

namespace clientbackend {

FileMedia::FileMedia(
    HANDLE backingFileHandle,
    uint64_t sizeBytes)
    : backingFile(backingFileHandle),
      mediaSizeBytes(sizeBytes)
{
}

FileMedia::~FileMedia()
{
    if (backingFile != INVALID_HANDLE_VALUE) {
        CloseHandle(backingFile);
        backingFile = INVALID_HANDLE_VALUE;
    }
}

bool FileMedia::readLocked(
    UINT64 offset,
    void* buffer,
    UINT32 length)
{
    DWORD bytesRead;
    std::lock_guard<std::mutex> ioGuard(backingFileIoLock);

    if (length == 0) {
        return true;
    }

    if (!seekLocked(offset)) {
        return false;
    }

    bytesRead = 0;
    if (!ReadFile(backingFile, buffer, length, &bytesRead, nullptr)) {
        return false;
    }

    return bytesRead == length;
}

bool FileMedia::writeLocked(
    UINT64 offset,
    const void* buffer,
    UINT32 length)
{
    DWORD bytesWritten;
    std::lock_guard<std::mutex> ioGuard(backingFileIoLock);

    if (length == 0) {
        return true;
    }

    if (!seekLocked(offset)) {
        return false;
    }

    bytesWritten = 0;
    if (!WriteFile(backingFile, buffer, length, &bytesWritten, nullptr)) {
        return false;
    }

    return bytesWritten == length;
}

uint64_t FileMedia::sizeBytes() const
{
    return mediaSizeBytes;
}

bool FileMedia::seekLocked(UINT64 offset)
{
    LARGE_INTEGER position;

    if (backingFile == INVALID_HANDLE_VALUE) {
        return false;
    }

    position.QuadPart = offset;
    return SetFilePointerEx(backingFile, position, nullptr, FILE_BEGIN) != FALSE;
}

} // namespace clientbackend
