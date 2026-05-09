#pragma once

#include <memory>
#include <vector>

#include <QString>
#include <QStringList>

#include "backend/types/types.h"

namespace clientbackend {
struct BackendContext;
}

struct BackendCreateDiskRequest {
    unsigned long targetId = 255;
    unsigned long long diskSizeBytes = 0;
    bool readOnly = false;
    clientbackend::MediaMode requestedMode = clientbackend::MediaMode::autoSelect;
};

class Backend final {
public:
    Backend();
    ~Backend();

    Backend(const Backend&) = delete;
    Backend& operator=(const Backend&) = delete;

    QString sessionStateText() const;
    QStringList initialLogLines() const;
    QStringList logLines() const;

    bool createManagedDisk(
        const BackendCreateDiskRequest& request,
        QString* outErrorText = nullptr);
    bool removeManagedDisk(
        unsigned long targetId,
        QString* outErrorText = nullptr);
    bool removeAllManagedDisks(
        bool closing,
        QString* outErrorText = nullptr);

    QString querySessionState() const;
    std::vector<clientbackend::ManagedDiskSnapshot> snapshotManagedDisks() const;
    bool queryBackendStats(
        clientbackend::BackendStatsSnapshot* outStats,
        QString* outErrorText = nullptr) const;
    bool queryDebugSnapshot(
        clientbackend::DebugSnapshot* outSnapshot,
        QString* outErrorText = nullptr) const;

private:
    std::unique_ptr<clientbackend::BackendContext> context;
};
