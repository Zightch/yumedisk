#include <iostream>

#include "config.h"
#include "runtime.h"

int main(
    int argc,
    char** argv)
{
    using namespace testapp;

    AppConfig config;
    ParseResult parseResult;
    CliContext context{};
    BackendCore::BackendStatsSnapshot stats{};
    std::wstring errorText;
    bool ok = false;

    parseResult = ParseArgs(argc, argv, &config);
    if (parseResult == ParseResult::Help) {
        PrintUsage();
        return 0;
    }
    if (parseResult != ParseResult::Ok) {
        PrintUsage();
        return 1;
    }

    context.Config = config;
    if (!context.Backend.open()) {
        const auto logLines = context.Backend.snapshotLogLines();
        if (!logLines.empty()) {
            std::wcerr << L"open backend session failed, reason=" << logLines.back() << std::endl;
        } else {
            std::wcerr << L"open backend session failed" << std::endl;
        }
        return 1;
    }

    std::wcout << L"queue_depth=" << config.QueueDepth
               << L", slot_bytes=" << config.WriteSlotBytes
               << L", sector_size=" << config.SectorSize
               << std::endl;
    std::wcout << L"state=ready(backendcore-host)" << std::endl;

    RunCommandLoop(&context);

    if (!context.Backend.queryBackendStats(&stats, &errorText)) {
        ok = false;
    } else {
        ok =
            (stats.commandFailures == 0) &&
            (stats.protocolFailures == 0);
    }

    (void)RemoveAllManagedDisks(&context, true);
    context.Backend.close();
    return ok ? 0 : 1;
}
