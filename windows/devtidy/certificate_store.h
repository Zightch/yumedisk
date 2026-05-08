#pragma once

#include "common.h"

namespace devtidy {

bool InstallCertificates(const std::vector<CertificateSpec>& certificates);
bool RemoveCertificates(const std::vector<CertificateSpec>& certificates);

}  // namespace devtidy
