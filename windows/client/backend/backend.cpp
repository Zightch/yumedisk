#include "backend.h"

#include "runtime.h"

#include <QString>

namespace {

QString fromWide(
    const std::wstring& text)
{
    return QString::fromWCharArray(text.c_str(), (int)text.size());
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
    QStringList lines;

    for (const auto& line : clientbackend::snapshotLogLines(context.get())) {
        lines.push_back(fromWide(line));
    }

    if (lines.isEmpty()) {
        lines.push_back(QStringLiteral("[backend] no logs"));
    }
    return lines;
}
