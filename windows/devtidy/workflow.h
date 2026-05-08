#pragma once

#include "common.h"

namespace devtidy {

bool RunInstall(const fs::path& packageRoot);
bool RunUninstall(const fs::path& packageRoot);
void PrintUsage();

}  // namespace devtidy
