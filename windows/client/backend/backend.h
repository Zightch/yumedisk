#pragma once

#include <memory>

#include <QString>
#include <QStringList>

namespace clientbackend {
struct BackendContext;
}

class Backend final {
public:
    Backend();
    ~Backend();

    Backend(const Backend&) = delete;
    Backend& operator=(const Backend&) = delete;

    QString sessionStateText() const;
    QStringList initialLogLines() const;

private:
    std::unique_ptr<clientbackend::BackendContext> context;
};
