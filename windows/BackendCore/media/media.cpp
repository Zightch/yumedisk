#include "media/media.h"

#include <mutex>
#include <shared_mutex>
#include <string>

#include "types/types.h"

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

bool initializeManagedDiskMedia(
    DiskRuntime* diskRuntime,
    MediaKind mediaKind,
    std::wstring* outReason)
{
    if (diskRuntime == nullptr) {
        setFailureReason(outReason, L"invalid-parameter");
        return false;
    }

    if (mediaKind == MediaKind::unknown) {
        setFailureReason(outReason, L"media-kind-missing");
        return false;
    }

    {
        std::unique_lock<std::shared_mutex> guard(diskRuntime->media.lock);
        if (diskRuntime->media.instance == nullptr) {
            setFailureReason(outReason, L"media-instance-missing");
            return false;
        }
        if (mediaKind == MediaKind::rawFile) {
            diskRuntime->metadata.diskSizeBytes = diskRuntime->media.instance->sizeBytes();
        }
    }

    diskRuntime->metadata.mediaKind = mediaKind;
    return true;
}

void cleanupManagedDiskMedia(
    DiskRuntime* diskRuntime)
{
    if (diskRuntime == nullptr) {
        return;
    }

    std::unique_lock<std::shared_mutex> guard(diskRuntime->media.lock);
    diskRuntime->staging.clearLocked();
    diskRuntime->media.instance.reset();
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
