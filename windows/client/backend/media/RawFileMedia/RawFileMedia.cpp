#include "backend/media/RawFileMedia/RawFileMedia.h"

namespace clientbackend {

namespace {

void setFailureReason(
    std::wstring* outReason,
    const wchar_t* reason)
{
    if (outReason != nullptr) {
        *outReason = reason;
    }
}

} // namespace

std::unique_ptr<RawFileMedia> RawFileMedia::open(
    const std::wstring& filePath,
    bool readOnly,
    ULONG sectorSize,
    std::wstring* outReason)
{
    HANDLE fileHandle;
    LARGE_INTEGER fileSize;
    DWORD desiredAccess;

    if (filePath.empty()) {
        setFailureReason(outReason, L"raw-file-path-required");
        return nullptr;
    }

    desiredAccess = GENERIC_READ;
    if (!readOnly) {
        desiredAccess |= GENERIC_WRITE;
    }

    fileHandle = CreateFileW(
        filePath.c_str(),
        desiredAccess,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        nullptr);
    if (fileHandle == INVALID_HANDLE_VALUE) {
        setFailureReason(outReason, L"raw-file-open-failed");
        return nullptr;
    }

    if (!GetFileSizeEx(fileHandle, &fileSize) || (fileSize.QuadPart <= 0)) {
        CloseHandle(fileHandle);
        setFailureReason(outReason, L"raw-file-size-invalid");
        return nullptr;
    }

    if (((uint64_t)fileSize.QuadPart % sectorSize) != 0) {
        CloseHandle(fileHandle);
        setFailureReason(outReason, L"raw-file-size-not-sector-aligned");
        return nullptr;
    }

    return std::unique_ptr<RawFileMedia>(
        new RawFileMedia(fileHandle, (uint64_t)fileSize.QuadPart));
}

RawFileMedia::RawFileMedia(
    HANDLE backingFile,
    uint64_t mediaSizeBytes)
    : FileMedia(backingFile, mediaSizeBytes)
{
}

} // namespace clientbackend
