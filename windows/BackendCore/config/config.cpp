#include "config/config.h"

#include "media/Media/Media.h"

namespace BackendCore {

namespace {

void assignErrorText(
    std::wstring* outErrorText,
    const wchar_t* text)
{
    if (outErrorText != nullptr) {
        *outErrorText = text;
    }
}

} // namespace

std::wstring mediaKindToText(MediaKind mediaKind)
{
    switch (mediaKind) {
    case MediaKind::denseMem:
        return L"denseMem";
    case MediaKind::sparseMem:
        return L"sparseMem";
    case MediaKind::rawFile:
        return L"rawFile";
    case MediaKind::unknown:
    default:
        return L"unknown";
    }
}

bool validateSessionConfig(
    const SessionConfig& sessionConfig,
    std::wstring* outErrorText)
{
    if (sessionConfig.heartbeatIntervalMs == 0u) {
        assignErrorText(outErrorText, L"invalid-heartbeat-interval-ms");
        return false;
    }

    if (sessionConfig.initialEventQueueCapacity == 0u) {
        assignErrorText(outErrorText, L"invalid-initial-event-queue-capacity");
        return false;
    }

    return true;
}

bool validateDiskConfig(
    const DiskConfig& diskConfig,
    std::wstring* outErrorText)
{
    if ((diskConfig.targetId != YUMEDISK_MAX_TARGETS) &&
        (diskConfig.targetId > YUMEDISK_MAX_USABLE_TARGET_ID)) {
        assignErrorText(outErrorText, L"invalid-target-id");
        return false;
    }

    if (diskConfig.sectorSize == 0u) {
        assignErrorText(outErrorText, L"invalid-sector-size");
        return false;
    }

    if ((diskConfig.diskSizeBytes == 0ull) ||
        ((diskConfig.diskSizeBytes % diskConfig.sectorSize) != 0ull)) {
        assignErrorText(outErrorText, L"invalid-disk-size-bytes");
        return false;
    }

    if (diskConfig.queueDepth == 0u) {
        assignErrorText(outErrorText, L"invalid-queue-depth");
        return false;
    }

    if (diskConfig.writeSlotBytes == 0u) {
        assignErrorText(outErrorText, L"invalid-write-slot-bytes");
        return false;
    }

    if (diskConfig.readWorkerCount == 0u) {
        assignErrorText(outErrorText, L"invalid-read-worker-count");
        return false;
    }

    if (diskConfig.writeWorkerCount == 0u) {
        assignErrorText(outErrorText, L"invalid-write-worker-count");
        return false;
    }

    if (diskConfig.ackBatchMaxRanges == 0u) {
        assignErrorText(outErrorText, L"invalid-ack-batch-max-ranges");
        return false;
    }

    return true;
}

bool validateCreateDiskInputs(
    const DiskConfig& diskConfig,
    MediaKind mediaKind,
    const Media* media,
    std::wstring* outErrorText)
{
    if (mediaKind == MediaKind::unknown) {
        assignErrorText(outErrorText, L"invalid-media-kind");
        return false;
    }

    if (media == nullptr) {
        assignErrorText(outErrorText, L"invalid-media-instance");
        return false;
    }

    if (!validateDiskConfig(diskConfig, outErrorText)) {
        return false;
    }

    if (media->sizeBytes() != diskConfig.diskSizeBytes) {
        assignErrorText(outErrorText, L"media-size-mismatch");
        return false;
    }

    return true;
}

AK_OPEN_PARAMS buildAkOpenParams(
    const SessionConfig& sessionConfig,
    AK_LOG_FN logFn,
    void* logCtx)
{
    AK_OPEN_PARAMS openParams{};

    openParams.HeartbeatIntervalMs = sessionConfig.heartbeatIntervalMs;
    openParams.InitialEventQueueCapacity = sessionConfig.initialEventQueueCapacity;
    openParams.LogFn = logFn;
    openParams.LogCtx = logCtx;
    return openParams;
}

AK_DISK_PARAMS buildAkDiskParams(
    const DiskConfig& diskConfig)
{
    AK_DISK_PARAMS diskParams{};

    diskParams.TargetId = diskConfig.targetId;
    diskParams.SectorSize = diskConfig.sectorSize;
    diskParams.DiskSizeBytes = diskConfig.diskSizeBytes;
    diskParams.QueueDepth = diskConfig.queueDepth;
    diskParams.WriteSlotBytes = diskConfig.writeSlotBytes;
    diskParams.ReadWorkerCount = diskConfig.readWorkerCount;
    diskParams.WriteWorkerCount = diskConfig.writeWorkerCount;
    diskParams.AckBatchMaxRanges = diskConfig.ackBatchMaxRanges;
    diskParams.ReadOnly = diskConfig.readOnly ? 1u : 0u;
    return diskParams;
}

} // namespace BackendCore

