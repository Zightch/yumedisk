#include "backend/media/media.h"

#include <exception>
#include <limits>
#include <memory>

#include "backend/config/config.h"
#include "backend/media/MemoryMedia/MemoryMedia.h"
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

bool initializeMemoryMedia(
    uint64_t diskSizeBytes,
    bool enforceDenseLimit,
    std::unique_ptr<Media>* outMedia,
    std::wstring* outReason)
{
    if (enforceDenseLimit && (diskSizeBytes > maxDenseMediaBytes)) {
        setFailureReason(outReason, L"dense-limit-exceeded");
        return false;
    }
    if (diskSizeBytes > (uint64_t)std::numeric_limits<size_t>::max()) {
        setFailureReason(outReason, enforceDenseLimit ? L"dense-size-overflow" : L"sparse-size-overflow");
        return false;
    }

    try {
        *outMedia = std::make_unique<MemoryMedia>((size_t)diskSizeBytes);
    } catch (const std::exception&) {
        setFailureReason(outReason, enforceDenseLimit ? L"dense-allocation-failed" : L"sparse-allocation-failed");
        return false;
    }

    return true;
}

bool initializeRawFileMedia(
    DiskRuntime* diskRuntime,
    std::unique_ptr<Media>* outMedia,
    std::wstring* outReason)
{
    *outMedia = RawFileMedia::open(
        diskRuntime->metadata.backingFilePath,
        diskRuntime->metadata.readOnly,
        diskRuntime->metadata.sectorSize,
        outReason);
    return *outMedia != nullptr;
}

} // namespace

bool initializeManagedDiskMedia(
    DiskRuntime* diskRuntime,
    MediaMode requestedMode,
    std::wstring* outReason)
{
    const MediaMode resolvedMode = resolveMediaMode(requestedMode, diskRuntime->metadata.diskSizeBytes);
    std::unique_ptr<Media> mediaInstance;

    if (resolvedMode == MediaMode::denseMem) {
        if (!initializeMemoryMedia(
                diskRuntime->metadata.diskSizeBytes,
                true,
                &mediaInstance,
                outReason)) {
            return false;
        }
        diskRuntime->metadata.backingFilePath.clear();
    } else if (resolvedMode == MediaMode::sparseMem) {
        if (!initializeMemoryMedia(
                diskRuntime->metadata.diskSizeBytes,
                false,
                &mediaInstance,
                outReason)) {
            return false;
        }
        diskRuntime->metadata.backingFilePath.clear();
    } else if (resolvedMode == MediaMode::rawFile) {
        if (!initializeRawFileMedia(diskRuntime, &mediaInstance, outReason)) {
            return false;
        }
        diskRuntime->metadata.diskSizeBytes = mediaInstance->sizeBytes();
    } else {
        setFailureReason(outReason, L"unsupported-media-mode");
        return false;
    }

    {
        std::unique_lock<std::shared_mutex> guard(diskRuntime->media.lock);
        diskRuntime->media.instance = std::move(mediaInstance);
    }

    diskRuntime->metadata.mode = resolvedMode;
    return true;
}

void cleanupManagedDiskMedia(
    DiskRuntime* diskRuntime)
{
    std::unique_lock<std::shared_mutex> guard(diskRuntime->media.lock);

    diskRuntime->staging.clearLocked();
    diskRuntime->media.instance.reset();
    diskRuntime->metadata.backingFilePath.clear();
}

bool writeMediaRangeLocked(
    DiskRuntime* diskRuntime,
    UINT64 offset,
    const void* buffer,
    UINT32 length)
{
    if ((diskRuntime == nullptr) || (diskRuntime->media.instance == nullptr)) {
        return false;
    }

    return diskRuntime->media.instance->writeLocked(offset, buffer, length);
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
        if ((diskRuntime->media.instance == nullptr) ||
            !diskRuntime->media.instance->readLocked(requestBegin, buffer, op->DataLength)) {
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
