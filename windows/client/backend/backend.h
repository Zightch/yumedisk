#pragma once

#include <QString>
#include <QStringList>

class Backend final {
public:
    Backend() = default;

    QString sessionStateText() const;
    QStringList initialLogLines() const;
};
