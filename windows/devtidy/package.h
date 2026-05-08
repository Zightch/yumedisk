#pragma once

#include "common.h"

namespace devtidy {

bool TryFindSingleInfPath(
    const DeviceSpec& spec,
    const fs::path& packageRoot,
    fs::path* infPath);

bool CollectPackageCertificates(
    const fs::path& packageRoot,
    CertificateMode* mode,
    std::vector<CertificateSpec>* certificates);

}  // namespace devtidy
