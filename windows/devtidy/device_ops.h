#pragma once

#include "common.h"

namespace devtidy {

bool EnsureDriverPackage(const DeviceSpec& spec, const fs::path& infPath);
bool UninstallDriverPackage(const DeviceSpec& spec, const fs::path& infPath);
bool EnsureUniqueDevice(const DeviceSpec& spec, const fs::path& infPath);
bool RemoveAllDevices(const DeviceSpec& spec);

}  // namespace devtidy
