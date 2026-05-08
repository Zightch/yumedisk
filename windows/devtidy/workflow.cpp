#include "workflow.h"

#include "certificate_store.h"
#include "device_ops.h"
#include "package.h"

#include <vector>

namespace devtidy {

bool RunInstall(const fs::path& packageRoot) {
    CertificateMode certificateMode = CertificateMode::Release;
    std::vector<CertificateSpec> certificates;
    if (!CollectPackageCertificates(packageRoot, &certificateMode, &certificates)) {
        return false;
    }
    if (certificateMode != CertificateMode::Release && !InstallCertificates(certificates)) {
        return false;
    }

    bool ok = true;
    std::vector<fs::path> infPaths;
    infPaths.reserve(kDeviceSpecs.size());

    for (const DeviceSpec& spec : kDeviceSpecs) {
        fs::path infPath;
        if (!TryFindSingleInfPath(spec, packageRoot, &infPath)) {
            ok = false;
            infPaths.push_back(fs::path());
            continue;
        }
        infPaths.push_back(infPath);

        if (!EnsureDriverPackage(spec, infPath)) {
            ok = false;
        }
    }

    for (size_t index = 0; index < kDeviceSpecs.size(); ++index) {
        if (infPaths[index].empty()) {
            continue;
        }

        if (!EnsureUniqueDevice(kDeviceSpecs[index], infPaths[index])) {
            ok = false;
        }
    }

    return ok;
}

bool RunUninstall(const fs::path& packageRoot) {
    CertificateMode certificateMode = CertificateMode::Release;
    std::vector<CertificateSpec> certificates;
    const bool haveCertificateLayout = CollectPackageCertificates(
        packageRoot,
        &certificateMode,
        &certificates);
    bool ok = true;
    if (!haveCertificateLayout) {
        ok = false;
    }

    for (const DeviceSpec& spec : kDeviceSpecs) {
        if (!RemoveAllDevices(spec)) {
            ok = false;
        }
    }

    std::vector<fs::path> infPaths;
    infPaths.reserve(kDeviceSpecs.size());
    for (const DeviceSpec& spec : kDeviceSpecs) {
        fs::path infPath;
        if (!TryFindSingleInfPath(spec, packageRoot, &infPath)) {
            ok = false;
            infPaths.push_back(fs::path());
            continue;
        }
        infPaths.push_back(infPath);
    }

    for (size_t index = 0; index < kDeviceSpecs.size(); ++index) {
        if (infPaths[index].empty()) {
            continue;
        }

        if (!UninstallDriverPackage(kDeviceSpecs[index], infPaths[index])) {
            ok = false;
        }
    }

    if (haveCertificateLayout &&
        certificateMode != CertificateMode::Release &&
        !RemoveCertificates(certificates)) {
        ok = false;
    }

    return ok;
}

void PrintUsage() {
    WriteInfoEvent(
        "usage",
        JsonObject({
            {"syntax", JsonText("devtidy [install|uninstall] [--package-root <path>]")}
        }));
}

}  // namespace devtidy
