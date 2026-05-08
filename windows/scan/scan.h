#pragma once

#include <Windows.h>

#include <cstdint>
#include <limits>
#include <string>
#include <vector>

namespace yumedisk::scan {

struct DiskIdentity {
    std::wstring Path;
    std::wstring Vendor;
    std::wstring Product;
    uint64_t LengthBytes = 0;
    DWORD DeviceNumber = std::numeric_limits<DWORD>::max();
};

std::vector<DiskIdentity> EnumerateVisibleYumeDisks();
std::wstring MakePhysicalDrivePath(DWORD device_number);

} // namespace yumedisk::scan
