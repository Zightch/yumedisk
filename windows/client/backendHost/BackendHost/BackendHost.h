#pragma once

#include <memory>
#include <vector>

#include <QString>
#include <QStringList>

namespace BackendCore {
struct BackendContext;
}

enum class BackendHostMediaMode {
    autoSelect,
    denseMem,
    sparseMem,
    rawFile
};

struct BackendHostCreateDiskRequest {
    QString capacityMiBText;
    QString targetIdText;
    QString sectorSizeText;
    QString queueDepthText;
    QString writeSlotBytesText;
    QString readWorkerCountText;
    QString writeWorkerCountText;
    QString ackBatchMaxRangesText;
    bool readOnly = false;
    BackendHostMediaMode requestedMode = BackendHostMediaMode::autoSelect;
    QString rawFilePath;
};

struct BackendHostManagedDiskSnapshot {
    unsigned long targetId = 0;
    QString lifecycleText;
    QString mediaText;
    QString visiblePathText;
};

struct BackendHostStatsSnapshot {
    unsigned long long heartbeatSent = 0;
    unsigned long long commandFailures = 0;
    unsigned long long protocolFailures = 0;
    unsigned long long eventsQueued = 0;
    unsigned long long eventsDropped = 0;
    unsigned long long diskCount = 0;
};

struct BackendHostSnapshot {
    QString sessionStateText;
    QStringList logLines;
    std::vector<BackendHostManagedDiskSnapshot> disks;
};

struct BackendHostDebugSnapshot {
    QString sessionStateText;
    BackendHostStatsSnapshot stats;
    std::vector<BackendHostManagedDiskSnapshot> disks;
};

class BackendHost final {
public:
    BackendHost();
    ~BackendHost();

    BackendHost(const BackendHost&) = delete;
    BackendHost& operator=(const BackendHost&) = delete;

    BackendHostSnapshot snapshot() const;

    bool createManagedDisk(
        const BackendHostCreateDiskRequest& request,
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
        BackendHostStatsSnapshot* outStats,
        QString* outErrorText = nullptr) const;
    bool queryDebugSnapshot(
        BackendHostDebugSnapshot* outSnapshot,
        QString* outErrorText = nullptr) const;

private:
    std::unique_ptr<BackendCore::BackendContext> context;
};

