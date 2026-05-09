#include "Backend.h"

#include <limits>

#include <QFileInfo>
#include "backend/runtime/runtime.h"
#include "backend/config/config.h"

#include <QString>

namespace {

constexpr qulonglong mibBytes = 1024ull * 1024ull;
constexpr unsigned long maxTargetId = 255;

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

clientbackend::MediaMode toClientMediaMode(
    BackendMediaMode mode)
{
    switch (mode) {
    case BackendMediaMode::denseMem:
        return clientbackend::MediaMode::denseMem;
    case BackendMediaMode::sparseMem:
        return clientbackend::MediaMode::sparseMem;
    case BackendMediaMode::rawFile:
        return clientbackend::MediaMode::rawFile;
    case BackendMediaMode::autoSelect:
    default:
        return clientbackend::MediaMode::autoSelect;
    }
}

QString mediaText(
    clientbackend::MediaMode mode)
{
    return fromWide(clientbackend::mediaModeToText(mode));
}

QString visiblePathText(
    const clientbackend::ManagedDiskSnapshot& snapshot)
{
    if (!snapshot.visiblePath.empty()) {
        return fromWide(snapshot.visiblePath);
    }

    if (!snapshot.physicalDrivePath.empty()) {
        return fromWide(snapshot.physicalDrivePath);
    }

    return QStringLiteral("<pending-enumeration>");
}

BackendManagedDiskSnapshot toBackendManagedDiskSnapshot(
    const clientbackend::ManagedDiskSnapshot& snapshot)
{
    BackendManagedDiskSnapshot result;

    result.targetId = snapshot.targetId;
    result.lifecycleText = fromWide(snapshot.lifecycleText);
    result.mediaText = mediaText(snapshot.mode);
    result.visiblePathText = visiblePathText(snapshot);
    return result;
}

bool tryBuildCreateDiskRequest(
    const BackendCreateDiskRequest& request,
    clientbackend::CreateDiskRequest* outRequest,
    QString* outErrorText)
{
    clientbackend::CreateDiskRequest backendRequest{};
    bool ok = false;
    const bool rawModeSelected = request.requestedMode == BackendMediaMode::rawFile;
    const QString capacityText = request.capacityMiBText.trimmed();
    const QString targetIdText = request.targetIdText.trimmed();
    const qulonglong maxCapacityMiB =
        std::numeric_limits<qulonglong>::max() / mibBytes;

    backendRequest.readOnly = request.readOnly;
    backendRequest.requestedMode = toClientMediaMode(request.requestedMode);

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

        backendRequest.diskSizeBytes = (qulonglong)rawFileInfo.size();
        if ((backendRequest.diskSizeBytes % clientbackend::defaultSectorSize) != 0) {
            if (outErrorText != nullptr) {
                *outErrorText = QStringLiteral("raw 文件大小必须按 4096 字节对齐");
            }
            return false;
        }

        backendRequest.rawFilePath = rawFileInfo.absoluteFilePath().toStdWString();
    } else {
        const qulonglong capacityMiB = capacityText.toULongLong(&ok);

        if (!ok || capacityMiB == 0) {
            if (outErrorText != nullptr) {
                *outErrorText = QStringLiteral("容量必须是大于 0 的 MiB 整数");
            }
            return false;
        }

        if (capacityMiB > maxCapacityMiB) {
            if (outErrorText != nullptr) {
                *outErrorText = QStringLiteral("容量超出当前支持范围");
            }
            return false;
        }

        backendRequest.diskSizeBytes = capacityMiB * mibBytes;
    }

    if (!targetIdText.isEmpty()) {
        const qulonglong parsedTargetId = targetIdText.toULongLong(&ok);
        if (!ok || parsedTargetId > maxTargetId) {
            if (outErrorText != nullptr) {
                *outErrorText = QStringLiteral("target id 必须是 0 到 255 之间的整数");
            }
            return false;
        }

        backendRequest.targetId = static_cast<unsigned long>(parsedTargetId);
    }

    if (outRequest != nullptr) {
        *outRequest = backendRequest;
    }
    return true;
}

}  // namespace

Backend::Backend()
    : context(std::make_unique<clientbackend::BackendContext>())
{
    (void)context->open();
}

Backend::~Backend()
{
    context->close();
}

BackendSnapshot Backend::snapshot() const {
    BackendSnapshot result;

    result.sessionStateText = fromWide(context->querySessionStateText());
    for (const auto& line : context->snapshotLogLines()) {
        result.logLines.push_back(fromWide(line));
    }
    if (result.logLines.isEmpty()) {
        result.logLines.push_back(QStringLiteral("[backend] no logs"));
    }
    for (const auto& snapshotItem : context->snapshotManagedDisks()) {
        result.disks.push_back(toBackendManagedDiskSnapshot(snapshotItem));
    }

    return result;
}

bool Backend::createManagedDisk(
    const BackendCreateDiskRequest& request,
    QString* outErrorText)
{
    clientbackend::CreateDiskRequest backendRequest{};
    std::wstring errorText;

    if (!tryBuildCreateDiskRequest(request, &backendRequest, outErrorText)) {
        return false;
    }

    if (!context->createManagedDisk(backendRequest, &errorText)) {
        assignErrorText(outErrorText, errorText);
        return false;
    }

    return true;
}

bool Backend::removeManagedDisk(
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

bool Backend::removeAllManagedDisks(
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

bool Backend::shutdown(
    QString* outErrorText)
{
    if (context == nullptr) {
        if (outErrorText != nullptr) {
            *outErrorText = QStringLiteral("backend-missing");
        }
        return false;
    }

    context->close();
    return true;
}

bool Backend::queryBackendStats(
    BackendStatsSnapshot* outStats,
    QString* outErrorText) const
{
    std::wstring errorText;
    clientbackend::BackendStatsSnapshot stats{};

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

bool Backend::queryDebugSnapshot(
    BackendDebugSnapshot* outSnapshot,
    QString* outErrorText) const
{
    std::wstring errorText;
    clientbackend::DebugSnapshot snapshot{};

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
        outSnapshot->disks.push_back(toBackendManagedDiskSnapshot(disk));
    }
    return true;
}
