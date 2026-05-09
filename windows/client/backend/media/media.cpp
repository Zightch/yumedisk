#include "backend/media/media.h"

#include <WinIoCtl.h>

#include <algorithm>
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

bool ensureSparseBackingDirectory(
    std::wstring* outDirectory)
{
    wchar_t tempPath[MAX_PATH];
    DWORD length;
    std::wstring directory;

    length = GetTempPathW(MAX_PATH, tempPath);
    if ((length == 0) || (length >= MAX_PATH)) {
        return false;
    }

    directory.assign(tempPath, length);
    while (!directory.empty() &&
           ((directory.back() == L'\\') || (directory.back() == L'/'))) {
        directory.pop_back();
    }
    directory += LR"(\YumeDiskClient)";

    if (!CreateDirectoryW(directory.c_str(), nullptr) &&
        (GetLastError() != ERROR_ALREADY_EXISTS)) {
        return false;
    }

    *outDirectory = directory;
    return true;
}

std::wstring buildSparseBackingPath(
    ULONG targetId)
{
    static std::atomic<UINT64> nonce{1};
    std::wstring directory;

    if (!ensureSparseBackingDirectory(&directory)) {
        return {};
    }

    return directory + L"\\target-" +
           std::to_wstring(targetId) + L"-" +
           std::to_wstring(GetCurrentProcessId()) + L"-" +
           std::to_wstring(nonce.fetch_add(1, std::memory_order_relaxed)) +
           L".bin";
}

bool seekSparseFileLocked(
    ManagedDisk* disk,
    UINT64 offset)
{
    LARGE_INTEGER position;

    if ((disk == nullptr) || (disk->sparseFile == INVALID_HANDLE_VALUE)) {
        return false;
    }

    position.QuadPart = offset;
    return SetFilePointerEx(disk->sparseFile, position, nullptr, FILE_BEGIN) != FALSE;
}

bool readSparseRangeLocked(
    ManagedDisk* disk,
    UINT64 offset,
    void* buffer,
    UINT32 length)
{
    DWORD bytesRead;
    std::lock_guard<std::mutex> ioGuard(disk->sparseIoLock);

    if (length == 0) {
        return true;
    }

    if (!seekSparseFileLocked(disk, offset)) {
        return false;
    }

    bytesRead = 0;
    if (!ReadFile(disk->sparseFile, buffer, length, &bytesRead, nullptr)) {
        return false;
    }

    return bytesRead == length;
}

bool writeSparseRangeLocked(
    ManagedDisk* disk,
    UINT64 offset,
    const void* buffer,
    UINT32 length)
{
    DWORD bytesWritten;
    std::lock_guard<std::mutex> ioGuard(disk->sparseIoLock);

    if (length == 0) {
        return true;
    }

    if (!seekSparseFileLocked(disk, offset)) {
        return false;
    }

    bytesWritten = 0;
    if (!WriteFile(disk->sparseFile, buffer, length, &bytesWritten, nullptr)) {
        return false;
    }

    return bytesWritten == length;
}

bool readBackingRangeLocked(
    ManagedDisk* disk,
    UINT64 offset,
    void* buffer,
    UINT32 length)
{
    if (disk->mode == MediaMode::dense) {
        (void)memcpy(
            buffer,
            disk->denseMedium.data() + (size_t)offset,
            length);
        return true;
    }

    if (disk->mode == MediaMode::sparse) {
        return readSparseRangeLocked(disk, offset, buffer, length);
    }

    return false;
}

bool writeBackingRangeLocked(
    ManagedDisk* disk,
    UINT64 offset,
    const void* buffer,
    UINT32 length)
{
    if (disk->mode == MediaMode::dense) {
        (void)memcpy(
            disk->denseMedium.data() + (size_t)offset,
            buffer,
            length);
        return true;
    }

    if (disk->mode == MediaMode::sparse) {
        return writeSparseRangeLocked(disk, offset, buffer, length);
    }

    return false;
}

bool initializeDenseMedia(
    ManagedDisk* disk,
    std::wstring* outReason)
{
    if (disk->diskSizeBytes > maxDenseMediaBytes) {
        setFailureReason(outReason, L"dense-limit-exceeded");
        return false;
    }
    if (disk->diskSizeBytes > (uint64_t)std::numeric_limits<size_t>::max()) {
        setFailureReason(outReason, L"dense-size-overflow");
        return false;
    }

    try {
        disk->denseMedium.resize((size_t)disk->diskSizeBytes, 0u);
    } catch (const std::exception&) {
        setFailureReason(outReason, L"dense-allocation-failed");
        return false;
    }

    disk->sparseFile = INVALID_HANDLE_VALUE;
    disk->sparseBackingPath.clear();
    return true;
}

bool initializeSparseMedia(
    ManagedDisk* disk,
    std::wstring* outReason)
{
    DWORD bytesReturned;
    HANDLE fileHandle;
    LARGE_INTEGER fileSize;
    const std::wstring path = buildSparseBackingPath(disk->targetId);

    if (path.empty()) {
        setFailureReason(outReason, L"sparse-dir-create-failed");
        return false;
    }

    fileHandle = CreateFileW(
        path.c_str(),
        GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr,
        CREATE_ALWAYS,
        FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE,
        nullptr);
    if (fileHandle == INVALID_HANDLE_VALUE) {
        setFailureReason(outReason, L"sparse-file-open-failed");
        return false;
    }

    bytesReturned = 0;
    if (!DeviceIoControl(
            fileHandle,
            FSCTL_SET_SPARSE,
            nullptr,
            0,
            nullptr,
            0,
            &bytesReturned,
            nullptr)) {
        CloseHandle(fileHandle);
        setFailureReason(outReason, L"sparse-file-mark-failed");
        return false;
    }

    fileSize.QuadPart = (LONGLONG)disk->diskSizeBytes;
    if (!SetFilePointerEx(fileHandle, fileSize, nullptr, FILE_BEGIN) ||
        !SetEndOfFile(fileHandle)) {
        CloseHandle(fileHandle);
        setFailureReason(outReason, L"sparse-file-size-failed");
        return false;
    }

    disk->denseMedium.clear();
    disk->sparseFile = fileHandle;
    disk->sparseBackingPath = path;
    return true;
}

} // namespace

size_t countStagedFragmentsLocked(
    const ManagedDisk* disk)
{
    size_t count;

    count = 0;
    for (const auto& entry : disk->stagedWrites) {
        count += entry.second.fragments.size();
    }
    return count;
}

bool initializeManagedDiskMedia(
    ManagedDisk* disk,
    MediaMode requestedMode,
    std::wstring* outReason)
{
    const MediaMode resolvedMode = resolveMediaMode(requestedMode, disk->diskSizeBytes);

    disk->mode = resolvedMode;
    if (resolvedMode == MediaMode::dense) {
        return initializeDenseMedia(disk, outReason);
    }
    if (resolvedMode == MediaMode::sparse) {
        return initializeSparseMedia(disk, outReason);
    }

    setFailureReason(outReason, L"unsupported-media-mode");
    return false;
}

void cleanupManagedDiskMedia(
    ManagedDisk* disk)
{
    std::unique_lock<std::shared_mutex> guard(disk->mediaLock);

    disk->stagedWrites.clear();
    disk->denseMedium.clear();
    disk->denseMedium.shrink_to_fit();

    if (disk->sparseFile != INVALID_HANDLE_VALUE) {
        CloseHandle(disk->sparseFile);
        disk->sparseFile = INVALID_HANDLE_VALUE;
    }
    disk->sparseBackingPath.clear();
}

AK_STATUS AK_CALL hostReadBytes(
    void* mediaCtx,
    const AK_READ_OP* op,
    void* outBuffer,
    UINT32* outDataLength)
{
    ManagedDisk* disk;
    unsigned char* buffer;
    UINT64 requestBegin;
    UINT64 requestEnd;

    disk = static_cast<ManagedDisk*>(mediaCtx);
    if ((disk == nullptr) || (op == nullptr) || (outBuffer == nullptr) || (outDataLength == nullptr)) {
        return AK_STATUS_INVALID_PARAMETER;
    }

    if (op->DataLength == 0) {
        *outDataLength = 0;
        return AK_STATUS_SUCCESS;
    }

    requestBegin = op->OffsetBytes;
    requestEnd = requestBegin + (UINT64)op->DataLength;
    if ((requestEnd < requestBegin) || (requestEnd > disk->diskSizeBytes)) {
        *outDataLength = 0;
        return AK_STATUS_INVALID_PARAMETER;
    }

    buffer = static_cast<unsigned char*>(outBuffer);
    {
        struct OverlaySlice {
            UINT64 ordinal;
            size_t destOffset;
            size_t sourceOffset;
            size_t length;
            const unsigned char* data;
        };
        std::vector<OverlaySlice> overlays;

        std::shared_lock<std::shared_mutex> guard(disk->mediaLock);
        if (!readBackingRangeLocked(disk, requestBegin, buffer, op->DataLength)) {
            *outDataLength = 0;
            return AK_STATUS_UNSUCCESSFUL;
        }

        for (const auto& stagedEntry : disk->stagedWrites) {
            for (const auto& fragmentEntry : stagedEntry.second.fragments) {
                const StagedFragment& fragment = fragmentEntry.second;
                UINT64 fragmentBegin;
                UINT64 fragmentEnd;
                UINT64 overlapBegin;
                UINT64 overlapEnd;
                OverlaySlice slice;

                fragmentBegin = fragment.diskOffsetBytes;
                fragmentEnd = fragmentBegin + (UINT64)fragment.data.size();
                if ((fragmentEnd <= requestBegin) || (fragmentBegin >= requestEnd)) {
                    continue;
                }

                overlapBegin = std::max(fragmentBegin, requestBegin);
                overlapEnd = std::min(fragmentEnd, requestEnd);
                if (overlapEnd <= overlapBegin) {
                    continue;
                }

                slice.ordinal = fragment.ordinal;
                slice.destOffset = (size_t)(overlapBegin - requestBegin);
                slice.sourceOffset = (size_t)(overlapBegin - fragmentBegin);
                slice.length = (size_t)(overlapEnd - overlapBegin);
                slice.data = fragment.data.data();
                overlays.push_back(slice);
            }
        }

        std::sort(
            overlays.begin(),
            overlays.end(),
            [](const OverlaySlice& left, const OverlaySlice& right) {
                return left.ordinal < right.ordinal;
            });

        for (const auto& slice : overlays) {
            (void)memcpy(
                buffer + slice.destOffset,
                slice.data + slice.sourceOffset,
                slice.length);
        }
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
    ManagedDisk* disk;
    UINT64 writeBegin;
    UINT64 writeEnd;

    disk = static_cast<ManagedDisk*>(mediaCtx);
    if ((disk == nullptr) || (op == nullptr)) {
        return AK_STATUS_INVALID_PARAMETER;
    }

    if (dataLength != op->DataLength) {
        return AK_STATUS_INVALID_PARAMETER;
    }

    writeBegin = op->OffsetBytes;
    writeEnd = writeBegin + (UINT64)dataLength;
    if ((writeEnd < writeBegin) || (writeEnd > disk->diskSizeBytes)) {
        return AK_STATUS_INVALID_PARAMETER;
    }

    {
        std::unique_lock<std::shared_mutex> guard(disk->mediaLock);
        StagedWriteRecord& record = disk->stagedWrites[op->EventId];
        StagedFragment fragment;

        if ((record.totalSeq != 0) && (record.totalSeq != op->TotalSeq)) {
            return AK_STATUS_INVALID_PARAMETER;
        }

        record.totalSeq = op->TotalSeq;
        fragment.seq = op->Seq;
        fragment.diskOffsetBytes = op->OffsetBytes;
        fragment.ordinal = disk->nextStageOrdinal;
        disk->nextStageOrdinal += 1;
        fragment.data.resize(dataLength, 0u);
        if ((dataLength != 0) && (dataBuffer != nullptr)) {
            (void)memcpy(fragment.data.data(), dataBuffer, dataLength);
        }

        record.fragments[op->Seq] = std::move(fragment);
    }

    return AK_STATUS_SUCCESS;
}

bool applyCommittedWrite(
    ManagedDisk* disk,
    UINT64 eventId)
{
    std::unique_lock<std::shared_mutex> guard(disk->mediaLock);
    const auto it = disk->stagedWrites.find(eventId);
    if (it == disk->stagedWrites.end()) {
        return true;
    }

    for (const auto& fragmentEntry : it->second.fragments) {
        const StagedFragment& fragment = fragmentEntry.second;
        const UINT64 endOffset = fragment.diskOffsetBytes + (UINT64)fragment.data.size();

        if ((endOffset < fragment.diskOffsetBytes) || (endOffset > disk->diskSizeBytes)) {
            return false;
        }

        if (!fragment.data.empty() &&
            !writeBackingRangeLocked(
                disk,
                fragment.diskOffsetBytes,
                fragment.data.data(),
                (UINT32)fragment.data.size())) {
            return false;
        }
    }

    disk->stagedWrites.erase(it);
    return true;
}

void discardStagedWrite(
    ManagedDisk* disk,
    UINT64 eventId)
{
    std::unique_lock<std::shared_mutex> guard(disk->mediaLock);
    disk->stagedWrites.erase(eventId);
}

} // namespace clientbackend
