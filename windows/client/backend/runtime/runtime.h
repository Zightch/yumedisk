#pragma once

#include <string>
#include <vector>

#include "backend/types/types.h"

namespace clientbackend {

bool openBackendContext(BackendContext* context);
void closeBackendContext(BackendContext* context);
std::wstring formatStatusHex(AK_STATUS status);
std::wstring formatVersionBe(UINT32 versionBe);

} // namespace clientbackend
