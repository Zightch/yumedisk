#include "backendHost/BackendHost/BackendHost.h"

#include <exception>
#include <limits>
#include <memory>

#include <QFileInfo>

#include "BackendCore/BackendCore.h"
#include "media/MemoryMedia/MemoryMedia.h"
#include "media/RawFileMedia/RawFileMedia.h"

namespace {

constexpr qulonglong mibBytes = 1024ull * 1024ull;
constexpr uint64_t maxDenseMediaBytes = 1024ull * 1024ull * 1024ull;

QString fromWide(
    const std::wstring& text)
{
    return QString::fromWCharArray(text.c_str(), (int)text.size());
}

void assignErrorText(
    QString* outErrorText,
    const std::wstring& errorText)
{
    if (outErrorText != nullptr) {
        *outErrorText = fromWide(errorText);
    }
}

BackendCore::MediaKind resolveMediaKind(
    BackendHostMediaMode requestedMode,
    uint64_t diskSizeBytes)
{
    switch (requestedMode) {
    case BackendHostMediaMode::denseMem:
        return BackendCore::MediaKind::denseMem;
    case BackendHostMediaMode::sparseMem:
        return BackendCore::MediaKind::sparseMem;
    case BackendHostMediaMode::rawFile:
        return BackendCore::MediaKind::rawFile;
    case BackendHostMediaMode::autoSelect:
    default:
        return diskSizeBytes <= maxDenseMediaBytes
            ? BackendCore::MediaKind::denseMem
            : BackendCore::MediaKind::sparseMem;
    }
}

QString mediaText(
    BackendCore::MediaKind mediaKind)
{
    return fromWide(BackendCore::mediaKindToText(mediaKind));
}

QString visiblePathText(
    const BackendCore::ManagedDiskSnapshot& snapshot)
{
    if (!snapshot.visiblePath.empty()) {
        return fromWide(snapshot.visiblePath);
    }

    if (!snapshot.physicalDrivePath.empty()) {
        return fromWide(snapshot.physicalDrivePath);
    }

    return QStringLiteral("<pending-enumeration>");
}

BackendHostManagedDiskSnapshot toBackendHostManagedDiskSnapshot(
    const BackendCore::ManagedDiskSnapshot& snapshot)
{
    BackendHostManagedDiskSnapshot result;

    result.targetId = snapshot.targetId;
    result.lifecycleText = fromWide(snapshot.lifecycleText);
    result.mediaText = mediaText(snapshot.mediaKind);
    result.visiblePathText = visiblePathText(snapshot);
    return result;
}

bool tryParsePositiveULongLong(
    const QString& text,
    qulonglong maxValue,
    const QString& errorText,
    qulonglong* outValue,
    QString* outErrorText)
{
    bool ok = false;
    const qulonglong parsedValue = text.toULongLong(&ok);

    if (!ok || parsedValue == 0 || parsedValue > maxValue) {
        if (outErrorText != nullptr) {
            *outErrorText = errorText;
        }
        return false;
    }

    if (outValue != nullptr) {
        *outValue = parsedValue;
    }
    return true;
}

bool tryParseOptionalUInt32(
    const QString& text,
    quint32 defaultValue,
    const QString& errorText,
    quint32* outValue,
    QString* outErrorText)
{
    if (text.isEmpty()) {
        if (outValue != nullptr) {
            *outValue = defaultValue;
        }
        return true;
    }

    qulonglong parsedValue = 0;
    if (!tryParsePositiveULongLong(
            text,
            std::numeric_limits<quint32>::max(),
            errorText,
            &parsedValue,
            outErrorText)) {
        return false;
    }

    if (outValue != nullptr) {
        *outValue = (quint32)parsedValue;
    }
    return true;
}

bool tryParseOptionalUInt16(
    const QString& text,
    quint16 defaultValue,
    const QString& errorText,
    quint16* outValue,
    QString* outErrorText)
{
    if (text.isEmpty()) {
        if (outValue != nullptr) {
            *outValue = defaultValue;
        }
        return true;
    }

    qulonglong parsedValue = 0;
    if (!tryParsePositiveULongLong(
            text,
            std::numeric_limits<quint16>::max(),
            errorText,
            &parsedValue,
            outErrorText)) {
        return false;
    }

    if (outValue != nullptr) {
        *outValue = (quint16)parsedValue;
    }
    return true;
}

bool tryParseOptionalTargetId(
    const QString& text,
    unsigned long* outTargetId,
    QString* outErrorText)
{
    if (text.isEmpty()) {
        if (outTargetId != nullptr) {
            *outTargetId = YUMEDISK_MAX_TARGETS;
        }
        return true;
    }

    bool ok = false;
    const qulonglong parsedTargetId = text.toULongLong(&ok);
    if (!ok || parsedTargetId > YUMEDISK_MAX_USABLE_TARGET_ID) {
        if (outErrorText != nullptr) {
            *outErrorText = QStringLiteral("target id 必须是 0 到 254 之间的整数");
        }
        return false;
    }

    if (outTargetId != nullptr) {
        *outTargetId = (unsigned long)parsedTargetId;
    }
    return true;
}

bool tryResolveDiskSizeBytes(
    const BackendHostCreateDiskRequest& request,
    uint64_t* outDiskSizeBytes,
    QString* outRawFilePath,
    QString* outErrorText)
{
    const bool rawModeSelected = request.requestedMode == BackendHostMediaMode::rawFile;

    if (rawModeSelected) {
        const QString rawFileText = request.rawFilePath.trimmed();
        const QFileInfo rawFileInfo(rawFileText);

        if (rawFileText.isEmpty()) {
            if (outErrorText != nullptr) {
                *outErrorText = QStringLiteral("raw 文件路径不能为空");
            }
            return false;
        }

        if (!rawFileInfo.exists() || !rawFileInfo.isFile()) {
            if (outErrorText != nullptr) {
                *outErrorText = QStringLiteral("raw 文件必须是已存在的普通文件");
            }
            return false;
        }

        if (rawFileInfo.size() <= 0) {
            if (outErrorText != nullptr) {
                *outErrorText = QStringLiteral("raw 文件大小必须大于 0");
            }
            return false;
        }

        const auto diskSizeBytes = (uint64_t)rawFileInfo.size();
        if ((diskSizeBytes % BackendCore::defaultSectorSize) != 0) {
            if (outErrorText != nullptr) {
                *outErrorText = QStringLiteral("raw 文件大小必须按 4096 字节对齐");
            }
            return false;
        }

        if (outDiskSizeBytes != nullptr) {
            *outDiskSizeBytes = diskSizeBytes;
        }
        if (outRawFilePath != nullptr) {
            *outRawFilePath = rawFileInfo.absoluteFilePath();
        }
        return true;
    }

    const QString capacityText = request.capacityMiBText.trimmed();
    const qulonglong maxCapacityMiB =
        std::numeric_limits<qulonglong>::max() / mibBytes;
    qulonglong capacityMiB = 0;

    if (!tryParsePositiveULongLong(
            capacityText,
            maxCapacityMiB,
            QStringLiteral("容量必须是大于 0 的 MiB 整数"),
            &capacityMiB,
            outErrorText)) {
        return false;
    }

    if (outDiskSizeBytes != nullptr) {
        *outDiskSizeBytes = capacityMiB * mibBytes;
    }
    if (outRawFilePath != nullptr) {
        outRawFilePath->clear();
    }
    return true;
}

bool tryBuildDiskConfig(
    const BackendHostCreateDiskRequest& request,
    BackendCore::DiskConfig* outDiskConfig,
    BackendCore::MediaKind* outMediaKind,
    QString* outRawFilePath,
    QString* outErrorText)
{
    BackendCore::DiskConfig diskConfig{};
    QString rawFilePath;
    uint64_t diskSizeBytes = 0;
    quint32 sectorSize = BackendCore::defaultSectorSize;

    if (!tryResolveDiskSizeBytes(
            request,
            &diskSizeBytes,
            &rawFilePath,
            outErrorText)) {
        return false;
    }

    const BackendCore::MediaKind mediaKind =
        resolveMediaKind(request.requestedMode, diskSizeBytes);

    if (!tryParseOptionalTargetId(
            request.targetIdText.trimmed(),
            &diskConfig.targetId,
            outErrorText)) {
        return false;
    }

    diskConfig.diskSizeBytes = diskSizeBytes;
    diskConfig.readOnly = request.readOnly;

    if (!tryParseOptionalUInt32(
            request.sectorSizeText.trimmed(),
            BackendCore::defaultSectorSize,
            QStringLiteral("sector size 必须是正整数"),
            &sectorSize,
            outErrorText)) {
        return false;
    }
    diskConfig.sectorSize = (ULONG)sectorSize;

    if (!tryParseOptionalUInt32(
            request.queueDepthText.trimmed(),
            BackendCore::defaultQueueDepth,
            QStringLiteral("queue depth 必须是正整数"),
            &diskConfig.queueDepth,
            outErrorText)) {
        return false;
    }

    if (!tryParseOptionalUInt32(
            request.writeSlotBytesText.trimmed(),
            BackendCore::defaultWriteSlotBytes,
            QStringLiteral("write slot bytes 必须是正整数"),
            &diskConfig.writeSlotBytes,
            outErrorText)) {
        return false;
    }

    if (!tryParseOptionalUInt16(
            request.readWorkerCountText.trimmed(),
            BackendCore::defaultReadWorkerCount,
            QStringLiteral("read worker count 必须是正整数"),
            &diskConfig.readWorkerCount,
            outErrorText)) {
        return false;
    }

    if (!tryParseOptionalUInt16(
            request.writeWorkerCountText.trimmed(),
            BackendCore::defaultWriteWorkerCount,
            QStringLiteral("write worker count 必须是正整数"),
            &diskConfig.writeWorkerCount,
            outErrorText)) {
        return false;
    }

    if (!tryParseOptionalUInt32(
            request.ackBatchMaxRangesText.trimmed(),
            BackendCore::defaultAckBatchMaxRanges,
            QStringLiteral("ack batch max ranges 必须是正整数"),
            &diskConfig.ackBatchMaxRanges,
            outErrorText)) {
        return false;
    }

    if (mediaKind == BackendCore::MediaKind::rawFile &&
        diskConfig.sectorSize != BackendCore::defaultSectorSize) {
        if (outErrorText != nullptr) {
            *outErrorText = QStringLiteral("rawFile 当前要求 sector size 固定为 4096");
        }
        return false;
    }

    if (outDiskConfig != nullptr) {
        *outDiskConfig = diskConfig;
    }
    if (outMediaKind != nullptr) {
        *outMediaKind = mediaKind;
    }
    if (outRawFilePath != nullptr) {
        *outRawFilePath = rawFilePath;
    }
    return true;
}

bool tryCreateMedia(
    const BackendCore::DiskConfig& diskConfig,
    BackendCore::MediaKind mediaKind,
    const QString& rawFilePath,
    std::unique_ptr<BackendCore::Media>* outMedia,
    QString* outErrorText)
{
    if (outMedia == nullptr) {
        if (outErrorText != nullptr) {
            *outErrorText = QStringLiteral("media 输出参数不能为空");
        }
        return false;
    }

    if (mediaKind == BackendCore::MediaKind::denseMem ||
        mediaKind == BackendCore::MediaKind::sparseMem) {
        if (diskConfig.diskSizeBytes > (uint64_t)std::numeric_limits<size_t>::max()) {
            if (outErrorText != nullptr) {
                *outErrorText = mediaKind == BackendCore::MediaKind::denseMem
                    ? QStringLiteral("denseMem 大小超出当前进程可分配范围")
                    : QStringLiteral("sparseMem 大小超出当前进程可分配范围");
            }
            return false;
        }

        try {
            *outMedia = std::make_unique<BackendCore::MemoryMedia>(
                (size_t)diskConfig.diskSizeBytes);
        } catch (const std::exception&) {
            if (outErrorText != nullptr) {
                *outErrorText = mediaKind == BackendCore::MediaKind::denseMem
                    ? QStringLiteral("denseMem 分配失败")
                    : QStringLiteral("sparseMem 分配失败");
            }
            return false;
        }

        return true;
    }

    if (mediaKind == BackendCore::MediaKind::rawFile) {
        std::wstring reason;
        *outMedia = BackendCore::RawFileMedia::open(
            rawFilePath.toStdWString(),
            diskConfig.readOnly,
            diskConfig.sectorSize,
            &reason);
        if (*outMedia == nullptr) {
            assignErrorText(outErrorText, reason);
            return false;
        }

        return true;
    }

    if (outErrorText != nullptr) {
        *outErrorText = QStringLiteral("不支持的介质类型");
    }
    return false;
}

} // namespace

BackendHost::BackendHost()
    : context(std::make_unique<BackendCore::BackendContext>())
{
    (void)context->open();
}

BackendHost::~BackendHost()
{
    context->close();
}

BackendHostSnapshot BackendHost::snapshot() const
{
    BackendHostSnapshot result;

    result.sessionStateText = fromWide(context->querySessionStateText());
    for (const auto& line : context->snapshotLogLines()) {
        result.logLines.push_back(fromWide(line));
    }
    if (result.logLines.isEmpty()) {
        result.logLines.push_back(QStringLiteral("[backend] no logs"));
    }
    for (const auto& snapshotItem : context->snapshotManagedDisks()) {
        result.disks.push_back(toBackendHostManagedDiskSnapshot(snapshotItem));
    }

    return result;
}

bool BackendHost::createManagedDisk(
    const BackendHostCreateDiskRequest& request,
    QString* outErrorText)
{
    BackendCore::DiskConfig diskConfig{};
    BackendCore::MediaKind mediaKind = BackendCore::MediaKind::unknown;
    QString rawFilePath;
    std::unique_ptr<BackendCore::Media> media;
    std::wstring errorText;

    if (!tryBuildDiskConfig(
            request,
            &diskConfig,
            &mediaKind,
            &rawFilePath,
            outErrorText)) {
        return false;
    }

    if (!tryCreateMedia(
            diskConfig,
            mediaKind,
            rawFilePath,
            &media,
            outErrorText)) {
        return false;
    }

    if (!context->createManagedDisk(
            diskConfig,
            mediaKind,
            std::move(media),
            &errorText)) {
        assignErrorText(outErrorText, errorText);
        return false;
    }

    return true;
}

bool BackendHost::removeManagedDisk(
    unsigned long targetId,
    QString* outErrorText)
{
    std::wstring errorText;

    if (!context->removeManagedDisk(targetId, &errorText)) {
        assignErrorText(outErrorText, errorText);
        return false;
    }

    return true;
}

bool BackendHost::removeAllManagedDisks(
    bool closing,
    QString* outErrorText)
{
    if (!context->removeAllManagedDisks(closing)) {
        if (outErrorText != nullptr) {
            *outErrorText = QStringLiteral("remove-all-failed");
        }
        return false;
    }

    return true;
}

bool BackendHost::shutdown(
    QString* outErrorText)
{
    if (context == nullptr) {
        if (outErrorText != nullptr) {
            *outErrorText = QStringLiteral("backend-host-missing");
        }
        return false;
    }

    context->close();
    return true;
}

bool BackendHost::queryBackendStats(
    BackendHostStatsSnapshot* outStats,
    QString* outErrorText) const
{
    std::wstring errorText;
    BackendCore::BackendStatsSnapshot stats{};

    if ((outStats == nullptr) || !context->queryBackendStats(&stats, &errorText)) {
        assignErrorText(outErrorText, errorText);
        return false;
    }

    outStats->heartbeatSent = stats.heartbeatSent;
    outStats->commandFailures = stats.commandFailures;
    outStats->protocolFailures = stats.protocolFailures;
    outStats->eventsQueued = stats.eventsQueued;
    outStats->eventsDropped = stats.eventsDropped;
    outStats->diskCount = stats.diskCount;
    return true;
}

bool BackendHost::queryDebugSnapshot(
    BackendHostDebugSnapshot* outSnapshot,
    QString* outErrorText) const
{
    std::wstring errorText;
    BackendCore::DebugSnapshot snapshot{};

    if ((outSnapshot == nullptr) || !context->queryDebugSnapshot(&snapshot, &errorText)) {
        assignErrorText(outErrorText, errorText);
        return false;
    }

    outSnapshot->sessionStateText = fromWide(snapshot.sessionStateText);
    outSnapshot->stats.heartbeatSent = snapshot.stats.heartbeatSent;
    outSnapshot->stats.commandFailures = snapshot.stats.commandFailures;
    outSnapshot->stats.protocolFailures = snapshot.stats.protocolFailures;
    outSnapshot->stats.eventsQueued = snapshot.stats.eventsQueued;
    outSnapshot->stats.eventsDropped = snapshot.stats.eventsDropped;
    outSnapshot->stats.diskCount = snapshot.stats.diskCount;
    outSnapshot->disks.clear();
    outSnapshot->disks.reserve(snapshot.disks.size());
    for (const auto& disk : snapshot.disks) {
        outSnapshot->disks.push_back(toBackendHostManagedDiskSnapshot(disk));
    }
    return true;
}

