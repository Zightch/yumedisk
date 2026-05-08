#pragma once

#include <QString>
#include <QStringList>

class ClientBackend final {
public:
    ClientBackend() = default;

    QString sessionStateText() const;
    QStringList initialLogLines() const;
};
