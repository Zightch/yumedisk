#pragma once

#include <string>
#include <vector>

#include "backend/types/types.h"

namespace clientbackend {

bool openBackendContext(BackendContext* context);
void closeBackendContext(BackendContext* context);

std::wstring formatStatusHex(AK_STATUS status);
std::wstring formatVersionBe(UINT32 versionBe);
std::wstring querySessionStateText(const BackendContext* context);
std::vector<std::wstring> snapshotLogLines(const BackendContext* context);
std::vector<ManagedDiskSnapshot> snapshotManagedDisks(const BackendContext* context);
bool queryBackendStats(
    const BackendContext* context,
    BackendStatsSnapshot* outStats,
    std::wstring* outErrorText = nullptr);
bool queryDebugSnapshot(
    const BackendContext* context,
    DebugSnapshot* outSnapshot,
    std::wstring* outErrorText = nullptr);

ULONG findFirstFreeTarget(BackendContext* context);

bool createManagedDisk(
    BackendContext* context,
    const CreateDiskRequest& request,
    std::wstring* outErrorText = nullptr);

bool removeManagedDisk(
    BackendContext* context,
    ULONG targetId,
    std::wstring* outErrorText = nullptr);

bool removeAllManagedDisks(
    BackendContext* context,
    bool closing);

} // namespace clientbackend
