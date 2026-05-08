#pragma once

#include <string>
#include <vector>

#include "types.h"

namespace clientbackend {

bool openBackendContext(BackendContext* context);
void closeBackendContext(BackendContext* context);

std::wstring formatStatusHex(AK_STATUS status);
std::wstring formatVersionBe(UINT32 versionBe);
std::wstring querySessionStateText(const BackendContext* context);
std::vector<std::wstring> snapshotLogLines(const BackendContext* context);

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
