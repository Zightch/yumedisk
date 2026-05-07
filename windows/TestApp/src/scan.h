#pragma once

#include <string>
#include <vector>

#include "types.h"

namespace testapp {

std::vector<DiskIdentity> EnumerateVisibleYumeDisks();
std::wstring MakePhysicalDrivePath(DWORD device_number);

} // namespace testapp
