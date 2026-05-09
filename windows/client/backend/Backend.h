#pragma once

#include <memory>
#include <vector>

#include <QString>
#include <QStringList>

namespace clientbackend {
struct BackendContext;
}

enum class BackendMediaMode {
    autoSelect,
    denseMem,
    sparseMem,
    rawFile
};

struct BackendCreateDiskRequest {
    QString capacityMiBText;
    QString targetIdText;
    bool readOnly = false;
    BackendMediaMode requestedMode = BackendMediaMode::autoSelect;
    QString rawFilePath;
};

struct BackendManagedDiskSnapshot {
    unsigned long targetId = 0;
    QString lifecycleText;
    QString mediaText;
    QString visiblePathText;
};

struct BackendStatsSnapshot {
    unsigned long long heartbeatSent = 0;
    unsigned long long commandFailures = 0;
    unsigned long long protocolFailures = 0;
    unsigned long long eventsQueued = 0;
    unsigned long long eventsDropped = 0;
    unsigned long long diskCount = 0;
};

struct BackendSnapshot {
    QString sessionStateText;
    QStringList logLines;
    std::vector<BackendManagedDiskSnapshot> disks;
};

struct BackendDebugSnapshot {
    QString sessionStateText;
    BackendStatsSnapshot stats;
    std::vector<BackendManagedDiskSnapshot> disks;
};

class Backend final {
public:
    Backend();
    ~Backend();

    Backend(const Backend&) = delete;
    Backend& operator=(const Backend&) = delete;

    BackendSnapshot snapshot() const;

    bool createManagedDisk(
        const BackendCreateDiskRequest& request,
        QString* outErrorText = nullptr);
    bool removeManagedDisk(
        unsigned long targetId,
        QString* outErrorText = nullptr);
    bool removeAllManagedDisks(
        bool closing,
        QString* outErrorText = nullptr);
    bool shutdown(
        QString* outErrorText = nullptr);

    bool queryBackendStats(
        BackendStatsSnapshot* outStats,
        QString* outErrorText = nullptr) const;
    bool queryDebugSnapshot(
        BackendDebugSnapshot* outSnapshot,
        QString* outErrorText = nullptr) const;

private:
    std::unique_ptr<clientbackend::BackendContext> context;
};
