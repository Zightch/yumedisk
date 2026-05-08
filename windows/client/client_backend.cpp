#include "client_backend.h"

#include <appkernel.h>

#include <QLatin1Char>

namespace {

QString FormatVersionBe(unsigned int versionBe) {
    return QStringLiteral("0x%1")
        .arg(versionBe, 8, 16, QLatin1Char('0'))
        .toUpper();
}

}  // namespace

QString ClientBackend::sessionStateText() const {
    return QStringLiteral("未接入宿主后端");
}

QStringList ClientBackend::initialLogLines() const {
    return {
        QStringLiteral("[shell] client window ready"),
        QStringLiteral("[shell] AppKernel SDK ready, version %1")
            .arg(FormatVersionBe(AK_VERSION_BE)),
        QStringLiteral("[shell] waiting for minimal backend host integration"),
    };
}
