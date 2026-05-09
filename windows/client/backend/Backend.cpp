#include "Backend.h"

#include "backend/runtime/runtime.h"

#include <QString>

namespace {

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

QString Backend::sessionStateText() const {
    return fromWide(context->querySessionStateText());
}

QStringList Backend::initialLogLines() const {
    return logLines();
}

QStringList Backend::logLines() const {
    QStringList lines;

    for (const auto& line : context->snapshotLogLines()) {
        lines.push_back(fromWide(line));
    }

    if (lines.isEmpty()) {
        lines.push_back(QStringLiteral("[backend] no logs"));
    }
    return lines;
}

bool Backend::createManagedDisk(
    const BackendCreateDiskRequest& request,
    QString* outErrorText)
{
    clientbackend::CreateDiskRequest backendRequest{};
    std::wstring errorText;

    backendRequest.targetId = request.targetId;
    backendRequest.diskSizeBytes = request.diskSizeBytes;
    backendRequest.readOnly = request.readOnly;
    backendRequest.requestedMode = request.requestedMode;
    backendRequest.rawFilePath = request.rawFilePath.toStdWString();

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

QString Backend::querySessionState() const {
    return fromWide(context->querySessionStateText());
}

std::vector<clientbackend::ManagedDiskSnapshot> Backend::snapshotManagedDisks() const {
    return context->snapshotManagedDisks();
}

bool Backend::queryBackendStats(
    clientbackend::BackendStatsSnapshot* outStats,
    QString* outErrorText) const
{
    std::wstring errorText;

    if (!context->queryBackendStats(outStats, &errorText)) {
        assignErrorText(outErrorText, errorText);
        return false;
    }

    return true;
}

bool Backend::queryDebugSnapshot(
    clientbackend::DebugSnapshot* outSnapshot,
    QString* outErrorText) const
{
    std::wstring errorText;

    if (!context->queryDebugSnapshot(outSnapshot, &errorText)) {
        assignErrorText(outErrorText, errorText);
        return false;
    }

    return true;
}
