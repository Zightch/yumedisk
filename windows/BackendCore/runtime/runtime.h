#pragma once

#include <memory>
#include <string>
#include <vector>

#include "appkernel.h"
#include "types/types.h"

namespace clientbackend {

class Media;

class BackendContext final {
public:
    BackendContext();
    ~BackendContext();

    BackendContext(const BackendContext&) = delete;
    BackendContext& operator=(const BackendContext&) = delete;

    void setSessionConfig(const SessionConfig& sessionConfig);
    SessionConfig sessionConfig() const;

    bool open();
    void close();

    std::wstring querySessionStateText() const;
    std::vector<std::wstring> snapshotLogLines() const;
    std::vector<ManagedDiskSnapshot> snapshotManagedDisks() const;
    bool queryBackendStats(
        BackendStatsSnapshot* outStats,
        std::wstring* outErrorText = nullptr) const;
    bool queryDebugSnapshot(
        DebugSnapshot* outSnapshot,
        std::wstring* outErrorText = nullptr) const;

    ULONG findFirstFreeTarget();
    bool createManagedDisk(
        DiskConfig diskConfig,
        MediaKind mediaKind,
        std::unique_ptr<Media> media,
        std::wstring* outErrorText = nullptr);
    bool removeManagedDisk(
        ULONG targetId,
        std::wstring* outErrorText = nullptr);
    bool removeAllManagedDisks(bool closing);

    void appendLog(const std::wstring& text);

private:
    struct Impl;
    friend struct RuntimeAccess;

    std::unique_ptr<Impl> impl;
};

bool openBackendContext(BackendContext* context);
void closeBackendContext(BackendContext* context);
std::wstring formatStatusHex(AK_STATUS status);
std::wstring formatVersionBe(UINT32 versionBe);

} // namespace clientbackend
