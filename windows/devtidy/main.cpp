#include "common.h"
#include "workflow.h"

namespace devtidy {

int RunMain(int argc, wchar_t** argv) {
    fs::path packageRoot;
    RunMode mode = RunMode::Install;
    bool modeSeen = false;

    for (int index = 1; index < argc; ++index) {
        const std::wstring arg = argv[index];
        if (arg == L"install") {
            if (modeSeen) {
                PrintUsage();
                return 1;
            }

            mode = RunMode::Install;
            modeSeen = true;
            continue;
        }
        if (arg == L"uninstall") {
            if (modeSeen) {
                PrintUsage();
                return 1;
            }

            mode = RunMode::Uninstall;
            modeSeen = true;
            continue;
        }
        if (arg == L"--package-root") {
            if (index + 1 >= argc) {
                PrintUsage();
                return 1;
            }

            packageRoot = fs::path(argv[++index]);
            continue;
        }
        if (arg == L"--help" || arg == L"-h") {
            PrintUsage();
            return 0;
        }

        PrintUsage();
        return 1;
    }

    if (packageRoot.empty()) {
        packageRoot = GetPackageRootFromExecutable();
    }

    if (packageRoot.empty()) {
        WriteErrorEvent(
            "package_root_not_found",
            JsonObject({
                {"error", JsonText("unable to resolve package root from executable path")},
                {"hint", JsonText("pass --package-root <path>")}
            }));
        return 1;
    }

    WriteInfoEvent(
        "package_root",
        JsonObject({
            {"package_root", JsonText(packageRoot.native())}
        }));

    const bool ok =
        (mode == RunMode::Install) ? RunInstall(packageRoot) : RunUninstall(packageRoot);

    WriteInfoEvent(
        "summary",
        JsonObject({
            {"ok", Json::Value(ok)}
        }));
    return ok ? 0 : 1;
}

}  // namespace devtidy

int wmain(int argc, wchar_t** argv) {
    return devtidy::RunMain(argc, argv);
}
