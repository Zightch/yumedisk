#include <Windows.h>

#include <iostream>

#include "config.h"
#include "runtime.h"

namespace {

HANDLE g_StopEvent = nullptr;

BOOL WINAPI ConsoleCtrlHandler(
    DWORD control_type)
{
    switch (control_type) {
    case CTRL_C_EVENT:
    case CTRL_BREAK_EVENT:
    case CTRL_CLOSE_EVENT:
    case CTRL_LOGOFF_EVENT:
    case CTRL_SHUTDOWN_EVENT:
        if (g_StopEvent != nullptr) {
            SetEvent(g_StopEvent);
        }
        return TRUE;

    default:
        return FALSE;
    }
}

} // namespace

int main(
    int argc,
    char** argv)
{
    using namespace testapp;

    AppConfig config;
    ParseResult parse_result;
    BackendContext backend{};
    AK_OPEN_PARAMS open_params{};
    AK_STATUS status;
    AK_SESSION_STATE session_state{};
    AK_SESSION_STATS session_stats{};
    bool ok;

    parse_result = ParseArgs(argc, argv, &config);
    if (parse_result == ParseResult::Help) {
        PrintUsage();
        return 0;
    }
    if (parse_result != ParseResult::Ok) {
        PrintUsage();
        return 1;
    }

    backend.Config = config;
    backend.StopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (backend.StopEvent == nullptr) {
        std::cerr << "create stop event failed, error=" << GetLastError() << std::endl;
        return 1;
    }

    g_StopEvent = backend.StopEvent;
    (void)SetConsoleCtrlHandler(ConsoleCtrlHandler, TRUE);

    open_params.HeartbeatIntervalMs = kHeartbeatIntervalMs;
    open_params.InitialEventQueueCapacity = kInitialEventQueueCapacity;
    open_params.LogFn = AppKernelLogCallback;
    open_params.LogCtx = &backend;

    status = AkOpen(&open_params, &backend.Session);
    if (status != AK_STATUS_SUCCESS) {
        const DWORD last_error = GetLastError();
        std::cerr << "open appkernel session failed, status="
                  << std::hex << std::uppercase << (unsigned long)status
                  << ", win32=" << last_error
                  << std::dec << std::endl;
        CloseHandle(backend.StopEvent);
        backend.StopEvent = nullptr;
        g_StopEvent = nullptr;
        return 1;
    }

    if (AkQuerySessionState(backend.Session, &session_state) != AK_STATUS_SUCCESS) {
        session_state.SessionId = 0;
    }

    std::wcout << L"control_session=" << session_state.SessionId << std::endl;
    std::wcout << L"appkernel_version=" << FormatVersionBe(session_state.AppKernelVersionBe)
               << L", kmdf_version=" << FormatVersionBe(session_state.KmdfVersionBe)
               << L", scsi_version=" << FormatVersionBe(session_state.ScsiVersionBe)
               << std::endl;
    std::wcout << L"queue_depth=" << config.QueueDepth
               << L", slot_bytes=" << config.WriteSlotBytes
               << L", sector_size=" << config.SectorSize
               << L", disk_bytes=" << config.DiskSizeBytes
               << L", default_media=" << MediaModeToText(config.DefaultMediaMode)
               << std::endl;
    std::wcout << L"state=ready(appkernel-host)" << std::endl;

    backend.EventThread = std::thread(RunEventLoop, &backend);

    RunCommandLoop(&backend);
    (void)WaitForSingleObject(backend.StopEvent, INFINITE);

    (void)RemoveAllManagedDisks(&backend, true);
    backend.Stop.store(true, std::memory_order_relaxed);
    if (backend.EventThread.joinable()) {
        backend.EventThread.join();
    }

    (void)AkQuerySessionStats(backend.Session, &session_stats);
    PrintBackendStats(&backend);

    ok =
        (session_stats.CommandFailures == 0) &&
        (session_stats.ProtocolFailures == 0);

    AkClose(backend.Session);
    backend.Session = nullptr;

    (void)SetConsoleCtrlHandler(ConsoleCtrlHandler, FALSE);
    CloseHandle(backend.StopEvent);
    backend.StopEvent = nullptr;
    g_StopEvent = nullptr;
    return ok ? 0 : 1;
}
