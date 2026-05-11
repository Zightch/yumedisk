#include "runtime.h"

#include <algorithm>
#include <iostream>
#include <sstream>

#include "config.h"
#include "media.h"

namespace testapp {

using YumeDisk::Scan::EnumerateVisibleYumeDisks;
using YumeDisk::Scan::MakePhysicalDrivePath;

namespace {

const wchar_t* ReadOnlyToText(
    bool readOnly)
{
    return readOnly ? L"true" : L"false";
}

const wchar_t* OnlineToText(
    bool online)
{
    return online ? L"true" : L"false";
}

size_t ComputeWorkerCount(
    size_t slotDepth,
    size_t targetSlotsPerWorker,
    size_t maxWorkers)
{
    size_t workerCount;

    if (slotDepth == 0) {
        return 1;
    }

    workerCount = (slotDepth + targetSlotsPerWorker - 1) / targetSlotsPerWorker;
    workerCount = std::max<size_t>(1, workerCount);
    workerCount = std::min(workerCount, maxWorkers);
    workerCount = std::min(workerCount, slotDepth);
    return workerCount;
}

void PrintRuntimeHelp()
{
    std::cout
        << "commands:\n"
        << "  help                         show this help\n"
        << "  query                        print BackendCore session state\n"
        << "  ct <disk-size-mb> [auto|dense|sparse] [true|false] [target]\n"
        << "                               create one disk target through BackendCore\n"
        << "                               true=system read-only, false=read-write\n"
        << "  rm <target>                  remove one disk target\n"
        << "  rm all                       remove all disk targets\n"
        << "  ls                           list managed targets and visible YumeDisk disks\n"
        << "  stats                        print aggregated BackendCore counters\n"
        << "  debug                        print host plus BackendCore snapshot\n"
        << "  exit                         close session and quit\n";
}

std::wstring MediaText(
    MediaMode mode)
{
    switch (mode) {
    case MediaMode::Dense:
        return L"dense";
    case MediaMode::Sparse:
        return L"sparse";
    case MediaMode::Auto:
    default:
        return L"auto";
    }
}

ManagedDiskLocalState FindLocalDiskState(
    const CliContext* context,
    ULONG targetId)
{
    const auto it = context->Disks.find(targetId);

    if (it == context->Disks.end()) {
        return {};
    }

    return it->second;
}

bool WaitForDiskReady(
    CliContext* context,
    ULONG targetId,
    BackendCore::ManagedDiskSnapshot* outSnapshot)
{
    const ULONGLONG startTick = GetTickCount64();

    for (;;) {
        for (const auto& disk : context->Backend.snapshotManagedDisks()) {
            if ((disk.targetId == targetId) && !disk.physicalDrivePath.empty()) {
                if (outSnapshot != nullptr) {
                    *outSnapshot = disk;
                }
                return true;
            }
        }

        if ((GetTickCount64() - startTick) >= kDiskArrivalTimeoutMs) {
            return false;
        }

        Sleep(kDiskArrivalPollMs);
    }
}

bool CreateManagedDisk(
    CliContext* context,
    const CreateDiskRequest& request)
{
    CreateDiskRequest resolvedRequest = request;
    CreatedMedia createdMedia;
    BackendCore::DiskConfig diskConfig{};
    BackendCore::ManagedDiskSnapshot snapshot{};
    std::wstring errorText;
    ManagedDiskLocalState localState;

    if (resolvedRequest.TargetId >= YUMEDISK_MAX_TARGETS) {
        resolvedRequest.TargetId = context->Backend.findFirstFreeTarget();
        if (resolvedRequest.TargetId >= YUMEDISK_MAX_TARGETS) {
            std::wcerr << L"create failed, reason=no-free-target" << std::endl;
            return false;
        }
    }

    if (context->Disks.find(resolvedRequest.TargetId) != context->Disks.end()) {
        std::wcerr << L"create failed, target already exists: " << resolvedRequest.TargetId << std::endl;
        return false;
    }

    if (!CreateManagedDiskMedia(
            resolvedRequest,
            &createdMedia,
            &errorText)) {
        std::wcerr << L"create failed, target=" << resolvedRequest.TargetId
                   << L", read_only=" << ReadOnlyToText(resolvedRequest.ReadOnly)
                   << L", media=" << MediaModeToText(resolvedRequest.RequestedMode)
                   << L", reason=" << errorText
                   << std::endl;
        return false;
    }

    localState.Mode = createdMedia.Mode;
    localState.BackingDescription = createdMedia.BackingDescription;
    localState.QueueDepth = (UINT32)context->Config.QueueDepth;
    localState.ReadWorkerCount = (UINT16)ComputeWorkerCount(
        context->Config.QueueDepth,
        kReadSlotsPerWorkerTarget,
        kMaxReadWorkersPerDisk);
    localState.WriteWorkerCount = (UINT16)ComputeWorkerCount(
        context->Config.QueueDepth,
        kWriteSlotsPerWorkerTarget,
        kMaxWriteWorkersPerDisk);

    diskConfig.targetId = resolvedRequest.TargetId;
    diskConfig.sectorSize = context->Config.SectorSize;
    diskConfig.diskSizeBytes = resolvedRequest.DiskSizeBytes;
    diskConfig.queueDepth = (UINT32)context->Config.QueueDepth;
    diskConfig.writeSlotBytes = (UINT32)context->Config.WriteSlotBytes;
    diskConfig.readWorkerCount = localState.ReadWorkerCount;
    diskConfig.writeWorkerCount = localState.WriteWorkerCount;
    diskConfig.ackBatchMaxRanges = (UINT32)context->Config.QueueDepth;
    diskConfig.readOnly = resolvedRequest.ReadOnly;

    if (!context->Backend.createManagedDisk(
            diskConfig,
            std::move(createdMedia.Instance),
            &errorText)) {
        std::wcerr << L"create failed, target=" << resolvedRequest.TargetId
                   << L", read_only=" << ReadOnlyToText(resolvedRequest.ReadOnly)
                   << L", status=" << errorText
                   << std::endl;
        return false;
    }

    context->Disks[resolvedRequest.TargetId] = localState;
    std::wcout << L"created target=" << resolvedRequest.TargetId
               << L", disk_bytes=" << resolvedRequest.DiskSizeBytes
               << L", read_only=" << ReadOnlyToText(resolvedRequest.ReadOnly)
               << L", media=" << MediaText(localState.Mode)
               << L", queue_depth=" << context->Config.QueueDepth
               << L", slot_bytes=" << context->Config.WriteSlotBytes
               << std::endl;

    if (WaitForDiskReady(context, resolvedRequest.TargetId, &snapshot)) {
        std::wcout << L"visible_path=" << snapshot.visiblePath
                   << L", physical_drive=" << snapshot.physicalDrivePath
                   << std::endl;
    } else {
        std::wcout << L"visible_path=<pending-enumeration>, target=" << resolvedRequest.TargetId << std::endl;
    }

    return true;
}

bool RemoveManagedDisk(
    CliContext* context,
    ULONG targetId)
{
    std::wstring errorText;

    if (!context->Backend.removeManagedDisk(targetId, &errorText)) {
        std::wcerr << L"remove failed, target=" << targetId
                   << L", status=" << errorText
                   << std::endl;
        return false;
    }

    context->Disks.erase(targetId);
    std::wcout << L"removed target=" << targetId << std::endl;
    return true;
}

void RunQuery(
    CliContext* context)
{
    BackendCore::BackendStatsSnapshot stats{};
    std::wstring errorText;

    std::wcout << context->Backend.querySessionStateText() << std::endl;
    if (!context->Backend.queryBackendStats(&stats, &errorText)) {
        std::wcerr << L"query session stats failed, status=" << errorText << std::endl;
        return;
    }

    std::wcout << L"session_heartbeat_sent=" << stats.heartbeatSent
               << L", session_command_failures=" << stats.commandFailures
               << L", session_protocol_failures=" << stats.protocolFailures
               << L", session_events_queued=" << stats.eventsQueued
               << L", session_events_dropped=" << stats.eventsDropped
               << std::endl;
}

void ListManagedDisks(
    CliContext* context)
{
    const auto managedDisks = context->Backend.snapshotManagedDisks();
    const auto visibleDisks = EnumerateVisibleYumeDisks();

    std::wcout << L"managed_target_count=" << managedDisks.size() << std::endl;
    for (const auto& disk : managedDisks) {
        const auto localState = FindLocalDiskState(context, disk.targetId);
        std::wcout << L"target=" << disk.targetId
                   << L", disk_bytes=" << disk.diskSizeBytes
                   << L", sector_size=" << disk.sectorSize
                   << L", read_only=" << ReadOnlyToText(disk.readOnly)
                   << L", media=" << MediaText(localState.Mode)
                   << L", backing=" << (localState.BackingDescription.empty() ? L"<unknown-backing>" : localState.BackingDescription)
                   << L", slot_engine=" << (disk.online ? L"running" : L"stopped")
                   << L", read_workers=" << localState.ReadWorkerCount
                   << L", write_workers=" << localState.WriteWorkerCount
                   << L", read_slot_depth=" << localState.QueueDepth
                   << L", write_slot_depth=" << localState.QueueDepth
                   << L", visible_path=" << (disk.visiblePath.empty() ? L"<pending-enumeration>" : disk.visiblePath)
                   << L", physical_drive=" << (disk.physicalDrivePath.empty() ? L"<pending-enumeration>" : disk.physicalDrivePath)
                   << std::endl;
    }

    std::wcout << L"visible_disk_count=" << visibleDisks.size() << std::endl;
    for (size_t index = 0; index < visibleDisks.size(); ++index) {
        const auto& disk = visibleDisks[index];
        std::wcout << L"visible_disk[" << index << L"]"
                   << L", path=" << disk.Path
                   << L", device_number=" << disk.DeviceNumber
                   << L", disk_bytes=" << disk.LengthBytes
                   << std::endl;
    }
}

} // namespace

std::wstring FormatVersionBe(
    UINT32 versionBe)
{
    std::wostringstream stream;

    stream << ((versionBe >> 24) & 0xffu)
           << L'.' << ((versionBe >> 16) & 0xffu)
           << L'.' << ((versionBe >> 8) & 0xffu)
           << L'.' << (versionBe & 0xffu);
    return stream.str();
}

void PrintBackendStats(
    CliContext* context)
{
    BackendCore::BackendStatsSnapshot stats{};
    std::wstring errorText;

    if (!context->Backend.queryBackendStats(&stats, &errorText)) {
        std::wcout << L"stats_query_failed=true, status=" << errorText << std::endl;
        return;
    }

    std::wcout << L"backend_heartbeat_sent=" << stats.heartbeatSent
               << L", backend_command_failures=" << stats.commandFailures
               << L", backend_protocol_failures=" << stats.protocolFailures
               << L", backend_events_queued=" << stats.eventsQueued
               << L", backend_events_dropped=" << stats.eventsDropped
               << L", backend_disk_count=" << stats.diskCount
               << std::endl;
}

void PrintDebugSnapshot(
    CliContext* context,
    const wchar_t* reason)
{
    BackendCore::DebugSnapshot snapshot{};
    std::wstring errorText;

    std::wcout << L"debug_snapshot reason=" << reason << std::endl;
    if (!context->Backend.queryDebugSnapshot(&snapshot, &errorText)) {
        std::wcerr << L"debug query failed, status=" << errorText << std::endl;
        return;
    }

    std::wcout << L"debug_session " << snapshot.sessionStateText << std::endl;
    std::wcout << L"debug_stats heartbeat_sent=" << snapshot.stats.heartbeatSent
               << L", command_failures=" << snapshot.stats.commandFailures
               << L", protocol_failures=" << snapshot.stats.protocolFailures
               << L", events_queued=" << snapshot.stats.eventsQueued
               << L", events_dropped=" << snapshot.stats.eventsDropped
               << L", disk_count=" << snapshot.stats.diskCount
               << std::endl;

    for (const auto& disk : snapshot.disks) {
        const auto localState = FindLocalDiskState(context, disk.targetId);
        std::wcout << L"debug_disk target=" << disk.targetId
                   << L", lifecycle=" << disk.lifecycleText
                   << L", read_only=" << ReadOnlyToText(disk.readOnly)
                   << L", media=" << MediaText(localState.Mode)
                   << L", backing=" << (localState.BackingDescription.empty() ? L"<unknown-backing>" : localState.BackingDescription)
                   << L", online=" << OnlineToText(disk.online)
                   << L", read_workers=" << localState.ReadWorkerCount
                   << L", write_workers=" << localState.WriteWorkerCount
                   << L", visible_path=" << (disk.visiblePath.empty() ? L"<pending-enumeration>" : disk.visiblePath)
                   << std::endl;
    }
}

void RunCommandLoop(
    CliContext* context)
{
    PrintRuntimeHelp();

    for (;;) {
        std::cout << "> " << std::flush;

        std::string line;
        std::istringstream input;
        std::string command;

        if (!std::getline(std::cin, line)) {
            break;
        }

        input.str(line);
        input >> command;
        if (command.empty()) {
            continue;
        }

        if (command == "help") {
            PrintRuntimeHelp();
            continue;
        }
        if (command == "query") {
            RunQuery(context);
            continue;
        }
        if (command == "ls") {
            ListManagedDisks(context);
            continue;
        }
        if (command == "stats") {
            PrintBackendStats(context);
            continue;
        }
        if (command == "debug") {
            PrintDebugSnapshot(context, L"manual");
            continue;
        }
        if ((command == "exit") || (command == "quit")) {
            break;
        }
        if (command == "ct") {
            CreateDiskRequest request{};
            std::string token;
            std::vector<std::string> tokens;
            std::wstring errorText;

            while (input >> token) {
                tokens.push_back(token);
            }

            if (!ParseCreateDiskCommand(context->Config, tokens, &request, &errorText)) {
                std::wcerr << L"create failed, reason=" << errorText << std::endl;
                continue;
            }

            (void)CreateManagedDisk(context, request);
            continue;
        }
        if (command == "rm") {
            std::string arg;
            ULONG targetId;

            if (!(input >> arg)) {
                std::wcerr << L"rm requires <target>|all" << std::endl;
                continue;
            }

            if (arg == "all") {
                (void)RemoveAllManagedDisks(context, false);
                continue;
            }

            targetId = 0;
            if (!ParseTargetToken(arg, &targetId)) {
                std::wcerr << L"invalid target: " << std::wstring(arg.begin(), arg.end()) << std::endl;
                continue;
            }

            (void)RemoveManagedDisk(context, targetId);
            continue;
        }

        std::wcerr << L"unknown command: " << std::wstring(command.begin(), command.end()) << std::endl;
    }
}

bool RemoveAllManagedDisks(
    CliContext* context,
    bool closing)
{
    if (!context->Backend.removeAllManagedDisks(closing)) {
        if (!closing) {
            std::wcerr << L"remove all failed" << std::endl;
        }
        return false;
    }

    context->Disks.clear();
    if (!closing) {
        std::wcout << L"removed_all=true" << std::endl;
    }
    return true;
}

} // namespace testapp
