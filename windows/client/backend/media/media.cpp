#include "backend/media/media.h"

#include <cstring>
#include <exception>
#include <limits>

#include "backend/config/config.h"

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

bool isFileBackedMode(
    MediaMode mode)
{
    return mode == MediaMode::rawFile;
}

bool seekBackingFileLocked(
    DiskRuntime* diskRuntime,
    UINT64 offset)
{
    LARGE_INTEGER position;

    if ((diskRuntime == nullptr) || (diskRuntime->media.backingFile == INVALID_HANDLE_VALUE)) {
        return false;
    }

    position.QuadPart = offset;
    return SetFilePointerEx(diskRuntime->media.backingFile, position, nullptr, FILE_BEGIN) != FALSE;
}

bool readBackingFileRangeLocked(
    DiskRuntime* diskRuntime,
    UINT64 offset,
    void* buffer,
    UINT32 length)
{
    DWORD bytesRead;
    std::lock_guard<std::mutex> ioGuard(diskRuntime->media.backingFileIoLock);

    if (length == 0) {
        return true;
    }

    if (!seekBackingFileLocked(diskRuntime, offset)) {
        return false;
    }

    bytesRead = 0;
    if (!ReadFile(diskRuntime->media.backingFile, buffer, length, &bytesRead, nullptr)) {
        return false;
    }

    return bytesRead == length;
}

bool writeBackingFileRangeLocked(
    DiskRuntime* diskRuntime,
    UINT64 offset,
    const void* buffer,
    UINT32 length)
{
    DWORD bytesWritten;
    std::lock_guard<std::mutex> ioGuard(diskRuntime->media.backingFileIoLock);

    if (length == 0) {
        return true;
    }

    if (!seekBackingFileLocked(diskRuntime, offset)) {
        return false;
    }

    bytesWritten = 0;
    if (!WriteFile(diskRuntime->media.backingFile, buffer, length, &bytesWritten, nullptr)) {
        return false;
    }

    return bytesWritten == length;
}

bool readBackingRangeLocked(
    DiskRuntime* diskRuntime,
    UINT64 offset,
    void* buffer,
    UINT32 length)
{
    if ((diskRuntime->metadata.mode == MediaMode::denseMem) || (diskRuntime->metadata.mode == MediaMode::sparseMem)) {
        (void)memcpy(
            buffer,
            diskRuntime->media.memory.data() + (size_t)offset,
            length);
        return true;
    }

    if (isFileBackedMode(diskRuntime->metadata.mode)) {
        return readBackingFileRangeLocked(diskRuntime, offset, buffer, length);
    }

    return false;
}

bool writeBackingRangeLocked(
    DiskRuntime* diskRuntime,
    UINT64 offset,
    const void* buffer,
    UINT32 length)
{
    if ((diskRuntime->metadata.mode == MediaMode::denseMem) || (diskRuntime->metadata.mode == MediaMode::sparseMem)) {
        (void)memcpy(
            diskRuntime->media.memory.data() + (size_t)offset,
            buffer,
            length);
        return true;
    }

    if (isFileBackedMode(diskRuntime->metadata.mode)) {
        return writeBackingFileRangeLocked(diskRuntime, offset, buffer, length);
    }

    return false;
}

bool initializeDenseMedia(
    DiskRuntime* diskRuntime,
    std::wstring* outReason)
{
    if (diskRuntime->metadata.diskSizeBytes > maxDenseMediaBytes) {
        setFailureReason(outReason, L"dense-limit-exceeded");
        return false;
    }
    if (diskRuntime->metadata.diskSizeBytes > (uint64_t)std::numeric_limits<size_t>::max()) {
        setFailureReason(outReason, L"dense-size-overflow");
        return false;
    }

    try {
        diskRuntime->media.memory.resize((size_t)diskRuntime->metadata.diskSizeBytes, 0u);
    } catch (const std::exception&) {
        setFailureReason(outReason, L"dense-allocation-failed");
        return false;
    }

    diskRuntime->media.backingFile = INVALID_HANDLE_VALUE;
    diskRuntime->metadata.backingFilePath.clear();
    return true;
}

bool initializeSparseMedia(
    DiskRuntime* diskRuntime,
    std::wstring* outReason)
{
    if (diskRuntime->metadata.diskSizeBytes > (uint64_t)std::numeric_limits<size_t>::max()) {
        setFailureReason(outReason, L"sparse-size-overflow");
        return false;
    }

    try {
        diskRuntime->media.memory.resize((size_t)diskRuntime->metadata.diskSizeBytes, 0u);
    } catch (const std::exception&) {
        setFailureReason(outReason, L"sparse-allocation-failed");
        return false;
    }

    diskRuntime->media.backingFile = INVALID_HANDLE_VALUE;
    diskRuntime->metadata.backingFilePath.clear();
    return true;
}

bool initializeRawMedia(
    DiskRuntime* diskRuntime,
    std::wstring* outReason)
{
    HANDLE fileHandle;
    LARGE_INTEGER fileSize;
    DWORD desiredAccess;

    if (diskRuntime->metadata.backingFilePath.empty()) {
        setFailureReason(outReason, L"raw-file-path-required");
        return false;
    }

    desiredAccess = GENERIC_READ;
    if (!diskRuntime->metadata.readOnly) {
        desiredAccess |= GENERIC_WRITE;
    }

    fileHandle = CreateFileW(
        diskRuntime->metadata.backingFilePath.c_str(),
        desiredAccess,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        nullptr);
    if (fileHandle == INVALID_HANDLE_VALUE) {
        setFailureReason(outReason, L"raw-file-open-failed");
        return false;
    }

    if (!GetFileSizeEx(fileHandle, &fileSize) || (fileSize.QuadPart <= 0)) {
        CloseHandle(fileHandle);
        setFailureReason(outReason, L"raw-file-size-invalid");
        return false;
    }

    diskRuntime->metadata.diskSizeBytes = (uint64_t)fileSize.QuadPart;
    if ((diskRuntime->metadata.diskSizeBytes % diskRuntime->metadata.sectorSize) != 0) {
        CloseHandle(fileHandle);
        setFailureReason(outReason, L"raw-file-size-not-sector-aligned");
        return false;
    }

    diskRuntime->media.memory.clear();
    diskRuntime->media.backingFile = fileHandle;
    return true;
}

} // namespace

bool initializeManagedDiskMedia(
    DiskRuntime* diskRuntime,
    MediaMode requestedMode,
    std::wstring* outReason)
{
    const MediaMode resolvedMode = resolveMediaMode(requestedMode, diskRuntime->metadata.diskSizeBytes);

    diskRuntime->metadata.mode = resolvedMode;
    if (resolvedMode == MediaMode::denseMem) {
        return initializeDenseMedia(diskRuntime, outReason);
    }
    if (resolvedMode == MediaMode::sparseMem) {
        return initializeSparseMedia(diskRuntime, outReason);
    }
    if (resolvedMode == MediaMode::rawFile) {
        return initializeRawMedia(diskRuntime, outReason);
    }

    setFailureReason(outReason, L"unsupported-media-mode");
    return false;
}

void cleanupManagedDiskMedia(
    DiskRuntime* diskRuntime)
{
    std::unique_lock<std::shared_mutex> guard(diskRuntime->media.lock);

    diskRuntime->staging.clearLocked();
    diskRuntime->media.memory.clear();
    diskRuntime->media.memory.shrink_to_fit();

    if (diskRuntime->media.backingFile != INVALID_HANDLE_VALUE) {
        CloseHandle(diskRuntime->media.backingFile);
        diskRuntime->media.backingFile = INVALID_HANDLE_VALUE;
    }
    diskRuntime->metadata.backingFilePath.clear();
}

bool writeMediaRangeLocked(
    DiskRuntime* diskRuntime,
    UINT64 offset,
    const void* buffer,
    UINT32 length)
{
    return writeBackingRangeLocked(diskRuntime, offset, buffer, length);
}

AK_STATUS AK_CALL hostReadBytes(
    void* mediaCtx,
    const AK_READ_OP* op,
    void* outBuffer,
    UINT32* outDataLength)
{
    DiskRuntime* diskRuntime;
    unsigned char* buffer;
    UINT64 requestBegin;
    UINT64 requestEnd;

    diskRuntime = static_cast<DiskRuntime*>(mediaCtx);
    if ((diskRuntime == nullptr) || (op == nullptr) || (outBuffer == nullptr) || (outDataLength == nullptr)) {
        return AK_STATUS_INVALID_PARAMETER;
    }

    if (op->DataLength == 0) {
        *outDataLength = 0;
        return AK_STATUS_SUCCESS;
    }

    requestBegin = op->OffsetBytes;
    requestEnd = requestBegin + (UINT64)op->DataLength;
    if ((requestEnd < requestBegin) || (requestEnd > diskRuntime->metadata.diskSizeBytes)) {
        *outDataLength = 0;
        return AK_STATUS_INVALID_PARAMETER;
    }

    buffer = static_cast<unsigned char*>(outBuffer);
    {
        std::shared_lock<std::shared_mutex> guard(diskRuntime->media.lock);
        if (!readBackingRangeLocked(diskRuntime, requestBegin, buffer, op->DataLength)) {
            *outDataLength = 0;
            return AK_STATUS_UNSUCCESSFUL;
        }

        diskRuntime->staging.overlayReadLocked(requestBegin, buffer, op->DataLength);
    }

    *outDataLength = op->DataLength;
    return AK_STATUS_SUCCESS;
}

AK_STATUS AK_CALL hostStageWrite(
    void* mediaCtx,
    const AK_WRITE_OP* op,
    const void* dataBuffer,
    UINT32 dataLength)
{
    DiskRuntime* diskRuntime;

    diskRuntime = static_cast<DiskRuntime*>(mediaCtx);
    if (diskRuntime == nullptr) {
        return AK_STATUS_INVALID_PARAMETER;
    }

    {
        std::unique_lock<std::shared_mutex> guard(diskRuntime->media.lock);
        return diskRuntime->staging.stageWriteLocked(
            op,
            dataBuffer,
            dataLength,
            diskRuntime->metadata.diskSizeBytes);
    }
}

} // namespace clientbackend
