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
    (void)clientbackend::openBackendContext(context.get());
}

Backend::~Backend()
{
    clientbackend::closeBackendContext(context.get());
}

QString Backend::sessionStateText() const {
    return fromWide(clientbackend::querySessionStateText(context.get()));
}

QStringList Backend::initialLogLines() const {
    return logLines();
}

QStringList Backend::logLines() const {
    QStringList lines;

    for (const auto& line : clientbackend::snapshotLogLines(context.get())) {
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

    if (!clientbackend::createManagedDisk(context.get(), backendRequest, &errorText)) {
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

    if (!clientbackend::removeManagedDisk(context.get(), targetId, &errorText)) {
        assignErrorText(outErrorText, errorText);
        return false;
    }

    return true;
}

bool Backend::removeAllManagedDisks(
    bool closing,
    QString* outErrorText)
{
    if (!clientbackend::removeAllManagedDisks(context.get(), closing)) {
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
    return removeAllManagedDisks(true, outErrorText);
}

QString Backend::querySessionState() const {
    return fromWide(clientbackend::querySessionStateText(context.get()));
}

std::vector<clientbackend::ManagedDiskSnapshot> Backend::snapshotManagedDisks() const {
    return clientbackend::snapshotManagedDisks(context.get());
}

bool Backend::queryBackendStats(
    clientbackend::BackendStatsSnapshot* outStats,
    QString* outErrorText) const
{
    std::wstring errorText;

    if (!clientbackend::queryBackendStats(context.get(), outStats, &errorText)) {
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

    if (!clientbackend::queryDebugSnapshot(context.get(), outSnapshot, &errorText)) {
        assignErrorText(outErrorText, errorText);
        return false;
    }

    return true;
}
