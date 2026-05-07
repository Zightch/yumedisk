#pragma once

#include <string>
#include <vector>

#include "types.h"

namespace testapp {

std::vector<DiskIdentity> EnumerateVisibleYumeDisks(const AppConfig& config);
std::wstring MakePhysicalDrivePath(DWORD device_number);

} // namespace testapp
