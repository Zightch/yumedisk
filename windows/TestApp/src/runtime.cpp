#include "runtime.h"

#include <algorithm>
#include <iomanip>
#include <iostream>
#include <sstream>

#include "config.h"
#include "media.h"
#include "scan.h"

namespace testapp {

using yumedisk::scan::EnumerateVisibleYumeDisks;
using yumedisk::scan::MakePhysicalDrivePath;

namespace {

const AK_MEDIA_OPS kMediaOps = {
    HostReadBytes,
    HostStageWrite
};

const wchar_t* ReadOnlyToText(
    bool read_only)
{
    return read_only ? L"true" : L"false";
}

std::vector<std::shared_ptr<ManagedDisk>> SnapshotManagedDisks(
    BackendContext* context)
{
    std::vector<std::shared_ptr<ManagedDisk>> disks;

    if (context == nullptr) {
        return disks;
    }

    std::lock_guard<std::mutex> guard(context->DisksLock);
    disks.reserve(context->Disks.size());
    for (const auto& entry : context->Disks) {
        disks.push_back(entry.second);
    }

    return disks;
}

std::shared_ptr<ManagedDisk> FindManagedDisk(
    BackendContext* context,
    ULONG target_id)
{
    std::lock_guard<std::mutex> guard(context->DisksLock);
    const auto it = context->Disks.find(target_id);
    if (it == context->Disks.end()) {
        return nullptr;
    }

    return it->second;
}

void InsertManagedDisk(
    BackendContext* context,
    const std::shared_ptr<ManagedDisk>& disk)
{
    std::lock_guard<std::mutex> guard(context->DisksLock);
    context->Disks[disk->TargetId] = disk;
}

void RemoveManagedDiskFromMap(
    BackendContext* context,
    ULONG target_id)
{
    std::lock_guard<std::mutex> guard(context->DisksLock);
    context->Disks.erase(target_id);
}

bool ManagedDiskExists(
    BackendContext* context,
    ULONG target_id)
{
    std::lock_guard<std::mutex> guard(context->DisksLock);
    return context->Disks.find(target_id) != context->Disks.end();
}

ULONG FindFirstFreeTarget(
    BackendContext* context)
{
    std::lock_guard<std::mutex> guard(context->DisksLock);

    for (ULONG target_id = YUMEDISK_MIN_TARGET_ID;
         target_id <= YUMEDISK_MAX_USABLE_TARGET_ID;
         ++target_id) {
        if (context->Disks.find(target_id) == context->Disks.end()) {
            return target_id;
        }
    }

    return YUMEDISK_MAX_TARGETS;
}

std::vector<std::wstring> SnapshotClaimedDiskPaths(
    BackendContext* context,
    ULONG excluded_target_id)
{
    std::vector<std::wstring> paths;

    std::lock_guard<std::mutex> guard(context->DisksLock);
    for (const auto& entry : context->Disks) {
        if (entry.first == excluded_target_id) {
            continue;
        }
        if (!entry.second->Identity.Path.empty()) {
            paths.push_back(entry.second->Identity.Path);
        }
    }

    return paths;
}

bool ContainsPath(
    const std::vector<std::wstring>& paths,
    const std::wstring& path)
{
    return std::find(paths.begin(), paths.end(), path) != paths.end();
}

bool ContainsVisibleDiskPath(
    const std::vector<DiskIdentity>& identities,
    const std::wstring& path)
{
    return std::any_of(
        identities.begin(),
        identities.end(),
        [&](const DiskIdentity& identity) {
            return identity.Path == path;
        });
}

bool TryRefreshManagedDiskIdentity(
    BackendContext* context,
    const std::shared_ptr<ManagedDisk>& disk,
    const std::vector<DiskIdentity>* baseline_visible_disks,
    DWORD timeout_ms)
{
    const auto claimed_paths = SnapshotClaimedDiskPaths(context, disk->TargetId);
    const ULONGLONG start_tick = GetTickCount64();

    for (;;) {
        const auto visible_disks = EnumerateVisibleYumeDisks();
        const DiskIdentity* selected = nullptr;

        for (const auto& identity : visible_disks) {
            if (ContainsPath(claimed_paths, identity.Path)) {
                continue;
            }
            if ((baseline_visible_disks != nullptr) &&
                ContainsVisibleDiskPath(*baseline_visible_disks, identity.Path)) {
                continue;
            }

            selected = &identity;
            break;
        }

        if ((selected == nullptr) && !visible_disks.empty()) {
            for (const auto& identity : visible_disks) {
                if (!ContainsPath(claimed_paths, identity.Path)) {
                    selected = &identity;
                    break;
                }
            }
        }

        if (selected != nullptr) {
            disk->Identity = *selected;
            return true;
        }

        if ((timeout_ms == 0) ||
            context->Stop.load(std::memory_order_relaxed) ||
            ((GetTickCount64() - start_tick) >= timeout_ms)) {
            return false;
        }

        Sleep(kDiskArrivalPollMs);
    }
}

size_t ComputeWorkerCount(
    size_t slot_depth,
    size_t target_slots_per_worker,
    size_t max_workers)
{
    size_t worker_count;

    if (slot_depth == 0) {
        return 1;
    }

    worker_count = (slot_depth + target_slots_per_worker - 1) / target_slots_per_worker;
    worker_count = std::max<size_t>(1, worker_count);
    worker_count = std::min(worker_count, max_workers);
    worker_count = std::min(worker_count, slot_depth);
    return worker_count;
}

std::wstring LifecycleToText(
    AK_LIFECYCLE_STATE lifecycle)
{
    switch (lifecycle) {
    case AkStateInit:
        return L"init";
    case AkStateStarting:
        return L"starting";
    case AkStateRunning:
        return L"running";
    case AkStateRemoving:
        return L"removing";
    case AkStateClosing:
        return L"closing";
    case AkStateClosed:
        return L"closed";
    case AkStateBroken:
        return L"broken";
    default:
        return L"unknown";
    }
}

std::wstring BackingDescription(
    const ManagedDisk& disk)
{
    if (disk.Mode == MediaMode::Sparse) {
        return disk.SparseBackingPath.empty() ? L"<sparse-pending>" : disk.SparseBackingPath;
    }

    return L"<dense-memory>";
}

void HandleAppKernelEvent(
    BackendContext* context,
    const AK_EVENT* event_record)
{
    std::shared_ptr<ManagedDisk> disk;

    if ((context == nullptr) || (event_record == nullptr)) {
        return;
    }

    if (event_record->Type == AkEventSessionBroken) {
        LogWide(
            context,
            L"session broken, status=" + FormatStatusHex(event_record->Status));
        context->Stop.store(true, std::memory_order_relaxed);
        if (context->StopEvent != nullptr) {
            SetEvent(context->StopEvent);
        }
        return;
    }

    disk = FindManagedDisk(context, event_record->TargetId);
    if (disk == nullptr) {
        return;
    }

    switch (event_record->Type) {
    case AkEventDiskOnline:
        (void)TryRefreshManagedDiskIdentity(context, disk, nullptr, 0);
        break;

    case AkEventDiskRemoved:
        break;

    case AkEventWriteFinalCommitted:
        if (!ApplyCommittedWrite(disk.get(), event_record->EventId)) {
            LogWide(
                context,
                L"commit write failed, target=" + std::to_wstring(event_record->TargetId) +
                    L", event=" + std::to_wstring(event_record->EventId));
        }
        break;

    case AkEventWriteFinalRejected:
        DiscardStagedWrite(disk.get(), event_record->EventId);
        break;

    default:
        break;
    }
}

void PrintRuntimeHelp()
{
    std::cout
        << "commands:\n"
        << "  help                         show this help\n"
        << "  query                        print AppKernel session state\n"
        << "  ct <disk-size-mb> [auto|dense|sparse] [true|false] [target]\n"
        << "                               create one disk target through AppKernel\n"
        << "                               true=system read-only, false=read-write\n"
        << "  rm <target>                  remove one disk target\n"
        << "  rm all                       remove all disk targets\n"
        << "  ls                           list managed targets and visible YumeDisk disks\n"
        << "  stats                        print aggregated AppKernel counters\n"
        << "  debug                        print app host plus AppKernel snapshot\n"
        << "  exit                         close session and quit\n";
}

void RunQuery(
    BackendContext* context)
{
    AK_SESSION_STATE session_state{};
    AK_SESSION_STATS session_stats{};
    AK_STATUS status;

    status = AkQuerySessionState(context->Session, &session_state);
    if (status != AK_STATUS_SUCCESS) {
        std::wcerr << L"query session state failed, status=" << FormatStatusHex(status) << std::endl;
        return;
    }

    status = AkQuerySessionStats(context->Session, &session_stats);
    if (status != AK_STATUS_SUCCESS) {
        std::wcerr << L"query session stats failed, status=" << FormatStatusHex(status) << std::endl;
        return;
    }

    std::wcout << L"session=" << session_state.SessionId
               << L", lifecycle=" << LifecycleToText(session_state.Lifecycle)
               << L", heartbeat_running=" << (session_state.HeartbeatRunning ? L"true" : L"false")
               << L", transport_ready=" << (session_state.TransportReady ? L"true" : L"false")
               << L", disk_count=" << session_state.DiskCount
               << L", appkernel_version=" << FormatVersionBe(session_state.AppKernelVersionBe)
               << L", kmdf_version=" << FormatVersionBe(session_state.KmdfVersionBe)
               << L", scsi_version=" << FormatVersionBe(session_state.ScsiVersionBe)
               << L", last_error=" << FormatStatusHex(session_state.LastError)
               << std::endl;
    std::wcout << L"session_heartbeat_sent=" << session_stats.HeartbeatSent
               << L", session_command_failures=" << session_stats.CommandFailures
               << L", session_protocol_failures=" << session_stats.ProtocolFailures
               << L", session_events_queued=" << session_stats.EventsQueued
               << L", session_events_dropped=" << session_stats.EventsDropped
               << std::endl;
}

void ListManagedDisks(
    BackendContext* context)
{
    const auto managed_disks = SnapshotManagedDisks(context);
    const auto visible_disks = EnumerateVisibleYumeDisks();

    std::wcout << L"managed_target_count=" << managed_disks.size() << std::endl;
    for (const auto& disk : managed_disks) {
        AK_DISK_STATE disk_state{};
        const std::wstring physical_drive = MakePhysicalDrivePath(disk->Identity.DeviceNumber);
        const bool have_state = (disk->Handle != nullptr) &&
            (AkQueryDiskState(disk->Handle, &disk_state) == AK_STATUS_SUCCESS);

        (void)TryRefreshManagedDiskIdentity(context, disk, nullptr, 0);
        std::wcout << L"target=" << disk->TargetId
                   << L", disk_bytes=" << disk->DiskSizeBytes
                   << L", sector_size=" << disk->SectorSize
                   << L", read_only=" << ReadOnlyToText(disk->ReadOnly)
                   << L", media=" << MediaModeToText(disk->Mode)
                   << L", backing=" << BackingDescription(*disk)
                   << L", slot_engine=" << ((have_state && (disk_state.Lifecycle == AkStateRunning)) ? L"running" : L"stopped")
                   << L", read_workers=" << disk->ReadWorkerCount
                   << L", write_workers=" << disk->WriteWorkerCount
                   << L", read_slot_depth=" << disk->SlotDepth
                   << L", write_slot_depth=" << disk->SlotDepth
                   << L", visible_path=" << (disk->Identity.Path.empty() ? L"<pending-enumeration>" : disk->Identity.Path)
                   << L", physical_drive=" << (physical_drive.empty() ? L"<pending-enumeration>" : physical_drive)
                   << std::endl;
    }

    std::wcout << L"visible_disk_count=" << visible_disks.size() << std::endl;
    for (size_t index = 0; index < visible_disks.size(); ++index) {
        const auto& disk = visible_disks[index];
        std::wcout << L"visible_disk[" << index << L"]"
                   << L", path=" << disk.Path
                   << L", device_number=" << disk.DeviceNumber
                   << L", disk_bytes=" << disk.LengthBytes
                   << std::endl;
    }
}

bool CreateManagedDisk(
    BackendContext* context,
    const CreateDiskRequest& request)
{
    std::shared_ptr<ManagedDisk> disk;
    AK_DISK_PARAMS params{};
    AK_DISK* handle;
    AK_STATUS status;
    std::wstring media_reason;
    const auto visible_disks_before_create = EnumerateVisibleYumeDisks();

    if (ManagedDiskExists(context, request.TargetId)) {
        std::wcerr << L"create failed, target already exists: " << request.TargetId << std::endl;
        return false;
    }

    disk = std::make_shared<ManagedDisk>();
    disk->Backend = context;
    disk->TargetId = request.TargetId;
    disk->SectorSize = context->Config.SectorSize;
    disk->DiskSizeBytes = request.DiskSizeBytes;
    disk->ReadOnly = request.ReadOnly;
    disk->SlotDepth = context->Config.QueueDepth;
    disk->ReadWorkerCount = ComputeWorkerCount(
        disk->SlotDepth,
        kReadSlotsPerWorkerTarget,
        kMaxReadWorkersPerDisk);
    disk->WriteWorkerCount = ComputeWorkerCount(
        disk->SlotDepth,
        kWriteSlotsPerWorkerTarget,
        kMaxWriteWorkersPerDisk);

    if (!InitializeManagedDiskMedia(disk.get(), request.RequestedMode, &media_reason)) {
        std::wcerr << L"create failed, target=" << request.TargetId
                   << L", read_only=" << ReadOnlyToText(request.ReadOnly)
                   << L", media=" << MediaModeToText(request.RequestedMode)
                   << L", reason=" << media_reason
                   << std::endl;
        return false;
    }

    InsertManagedDisk(context, disk);

    params.TargetId = request.TargetId;
    params.SectorSize = context->Config.SectorSize;
    params.DiskSizeBytes = request.DiskSizeBytes;
    params.QueueDepth = (UINT32)context->Config.QueueDepth;
    params.WriteSlotBytes = (UINT32)context->Config.WriteSlotBytes;
    params.ReadWorkerCount = (UINT16)disk->ReadWorkerCount;
    params.WriteWorkerCount = (UINT16)disk->WriteWorkerCount;
    params.AckBatchMaxRanges = (UINT32)context->Config.QueueDepth;
    params.ReadOnly = request.ReadOnly ? 1u : 0u;

    handle = nullptr;
    status = AkCreateDisk(context->Session, &params, &kMediaOps, disk.get(), &handle);
    if (status != AK_STATUS_SUCCESS) {
        RemoveManagedDiskFromMap(context, request.TargetId);
        CleanupManagedDiskMedia(disk.get());
        std::wcerr << L"create failed, target=" << request.TargetId
                   << L", read_only=" << ReadOnlyToText(request.ReadOnly)
                   << L", status=" << FormatStatusHex(status) << std::endl;
        return false;
    }

    disk->Handle = handle;
    std::wcout << L"created target=" << request.TargetId
               << L", disk_bytes=" << request.DiskSizeBytes
               << L", read_only=" << ReadOnlyToText(disk->ReadOnly)
               << L", media=" << MediaModeToText(disk->Mode)
               << L", queue_depth=" << context->Config.QueueDepth
               << L", slot_bytes=" << context->Config.WriteSlotBytes
               << std::endl;
    if (TryRefreshManagedDiskIdentity(context, disk, &visible_disks_before_create, kDiskArrivalTimeoutMs)) {
        std::wcout << L"visible_path=" << disk->Identity.Path
                   << L", physical_drive=" << MakePhysicalDrivePath(disk->Identity.DeviceNumber)
                   << std::endl;
    } else {
        std::wcout << L"visible_path=<pending-enumeration>, target=" << request.TargetId << std::endl;
    }

    return true;
}

bool RemoveManagedDisk(
    BackendContext* context,
    ULONG target_id)
{
    const auto disk = FindManagedDisk(context, target_id);
    AK_STATUS status;

    if (disk == nullptr) {
        std::wcerr << L"remove failed, target not found: " << target_id << std::endl;
        return false;
    }

    if (disk->Handle == nullptr) {
        CleanupManagedDiskMedia(disk.get());
        RemoveManagedDiskFromMap(context, target_id);
        std::wcout << L"removed target=" << target_id << std::endl;
        return true;
    }

    status = AkRemoveDisk(disk->Handle);
    if (status != AK_STATUS_SUCCESS) {
        std::wcerr << L"remove failed, target=" << target_id
                   << L", status=" << FormatStatusHex(status) << std::endl;
        return false;
    }

    disk->Handle = nullptr;
    CleanupManagedDiskMedia(disk.get());
    RemoveManagedDiskFromMap(context, target_id);
    std::wcout << L"removed target=" << target_id << std::endl;
    return true;
}

} // namespace

std::wstring FormatStatusHex(
    AK_STATUS status)
{
    std::wostringstream stream;

    stream << L"0x"
           << std::uppercase
           << std::hex
           << std::setw(8)
           << std::setfill(L'0')
           << (unsigned long)status;
    return stream.str();
}

std::wstring FormatVersionBe(
    UINT32 version_be)
{
    std::wostringstream stream;

    stream << ((version_be >> 24) & 0xffu)
           << L'.' << ((version_be >> 16) & 0xffu)
           << L'.' << ((version_be >> 8) & 0xffu)
           << L'.' << (version_be & 0xffu);
    return stream.str();
}

void LogWide(
    BackendContext* context,
    const std::wstring& text)
{
    if (context == nullptr) {
        return;
    }

    std::lock_guard<std::mutex> guard(context->LogLock);
    std::wcerr << text << std::endl;
}

VOID AK_CALL AppKernelLogCallback(
    void* log_ctx,
    INT level,
    const char* text)
{
    BackendContext* context;
    std::ostringstream stream;

    context = static_cast<BackendContext*>(log_ctx);
    stream << "[AppKernel][" << level << "] " << (text == nullptr ? "" : text);

    std::lock_guard<std::mutex> guard(context->LogLock);
    std::cerr << stream.str() << std::endl;
}

void PrintBackendStats(
    BackendContext* context)
{
    AK_SESSION_STATS session_stats{};
    AK_STATUS session_status;
    UINT64 read_slot_posts;
    UINT64 read_slot_completions;
    UINT64 read_ack_commands;
    UINT64 write_slot_posts;
    UINT64 write_slot_completions;
    UINT64 write_ack_flushes;
    UINT64 write_ack_ranges;
    UINT64 write_ack_range_failures;
    UINT64 final_write_committed;
    UINT64 final_write_rejected;

    session_status = AkQuerySessionStats(context->Session, &session_stats);
    if (session_status != AK_STATUS_SUCCESS) {
        std::wcout << L"stats_query_failed=true, status=" << FormatStatusHex(session_status) << std::endl;
        return;
    }

    read_slot_posts = 0;
    read_slot_completions = 0;
    read_ack_commands = 0;
    write_slot_posts = 0;
    write_slot_completions = 0;
    write_ack_flushes = 0;
    write_ack_ranges = 0;
    write_ack_range_failures = 0;
    final_write_committed = 0;
    final_write_rejected = 0;

    for (const auto& disk : SnapshotManagedDisks(context)) {
        AK_DISK_STATS disk_stats{};
        if (disk->Handle == nullptr) {
            continue;
        }
        if (AkQueryDiskStats(disk->Handle, &disk_stats) != AK_STATUS_SUCCESS) {
            continue;
        }

        read_slot_posts += disk_stats.ReadSlotPosts;
        read_slot_completions += disk_stats.ReadSlotCompletions;
        read_ack_commands += disk_stats.ReadAckCommands;
        write_slot_posts += disk_stats.WriteSlotPosts;
        write_slot_completions += disk_stats.WriteSlotCompletions;
        write_ack_flushes += disk_stats.WriteAckFlushes;
        write_ack_ranges += disk_stats.WriteAckRanges;
        write_ack_range_failures += disk_stats.WriteAckRangeFailures;
        final_write_committed += disk_stats.FinalWriteCommitted;
        final_write_rejected += disk_stats.FinalWriteRejected;
    }

    std::wcout << L"backend_read_slot_posts=" << read_slot_posts
               << L", backend_read_slot_completions=" << read_slot_completions
               << L", backend_read_ack_commands=" << read_ack_commands
               << L", backend_write_slot_posts=" << write_slot_posts
               << L", backend_write_slot_completions=" << write_slot_completions
               << L", backend_flushed_ack_commands=" << write_ack_flushes
               << L", backend_flushed_ack_ranges=" << write_ack_ranges
               << L", backend_write_ack_range_failures=" << write_ack_range_failures
               << L", backend_final_write_committed=" << final_write_committed
               << L", backend_final_write_rejected=" << final_write_rejected
               << L", backend_command_failures=" << session_stats.CommandFailures
               << L", backend_protocol_failures=" << session_stats.ProtocolFailures
               << std::endl;
}

void PrintDebugSnapshot(
    BackendContext* context,
    const wchar_t* reason)
{
    AK_SESSION_STATE session_state{};
    const auto disks = SnapshotManagedDisks(context);

    std::wcout << L"debug_snapshot reason=" << reason << std::endl;
    PrintBackendStats(context);

    if (AkQuerySessionState(context->Session, &session_state) == AK_STATUS_SUCCESS) {
        std::wcout << L"debug_session session=" << session_state.SessionId
                   << L", lifecycle=" << LifecycleToText(session_state.Lifecycle)
                   << L", heartbeat_running=" << (session_state.HeartbeatRunning ? L"true" : L"false")
                   << L", transport_ready=" << (session_state.TransportReady ? L"true" : L"false")
                   << L", disk_count=" << session_state.DiskCount
                   << L", appkernel_version=" << FormatVersionBe(session_state.AppKernelVersionBe)
                   << L", kmdf_version=" << FormatVersionBe(session_state.KmdfVersionBe)
                   << L", scsi_version=" << FormatVersionBe(session_state.ScsiVersionBe)
                   << L", last_error=" << FormatStatusHex(session_state.LastError)
                   << std::endl;
    }

    for (const auto& disk : disks) {
        AK_DISK_STATE disk_state{};
        size_t staged_write_count;
        size_t staged_fragment_count;

        {
            std::shared_lock<std::shared_mutex> guard(disk->MediaLock);
            staged_write_count = disk->StagedWrites.size();
            staged_fragment_count = CountStagedFragmentsLocked(disk.get());
        }

        if ((disk->Handle != nullptr) &&
            (AkQueryDiskState(disk->Handle, &disk_state) == AK_STATUS_SUCCESS)) {
            std::wcout << L"debug_disk target=" << disk->TargetId
                       << L", lifecycle=" << LifecycleToText(disk_state.Lifecycle)
                       << L", read_only=" << ReadOnlyToText(disk->ReadOnly)
                       << L", media=" << MediaModeToText(disk->Mode)
                       << L", backing=" << BackingDescription(*disk)
                       << L", read_workers_running=" << (disk_state.ReadWorkersRunning ? L"true" : L"false")
                       << L", write_workers_running=" << (disk_state.WriteWorkersRunning ? L"true" : L"false")
                       << L", ack_flusher_running=" << (disk_state.AckFlusherRunning ? L"true" : L"false")
                       << L", read_workers=" << disk->ReadWorkerCount
                       << L", write_workers=" << disk->WriteWorkerCount
                       << L", staged_writes=" << staged_write_count
                       << L", staged_fragments=" << staged_fragment_count
                       << L", visible_path=" << (disk->Identity.Path.empty() ? L"<pending-enumeration>" : disk->Identity.Path)
                       << std::endl;
        } else {
            std::wcout << L"debug_disk target=" << disk->TargetId
                       << L", lifecycle=<unknown>"
                       << L", read_only=" << ReadOnlyToText(disk->ReadOnly)
                       << L", media=" << MediaModeToText(disk->Mode)
                       << L", backing=" << BackingDescription(*disk)
                       << L", read_workers=" << disk->ReadWorkerCount
                       << L", write_workers=" << disk->WriteWorkerCount
                       << L", staged_writes=" << staged_write_count
                       << L", staged_fragments=" << staged_fragment_count
                       << L", visible_path=" << (disk->Identity.Path.empty() ? L"<pending-enumeration>" : disk->Identity.Path)
                       << std::endl;
        }
    }
}

void RunEventLoop(
    BackendContext* context)
{
    while (!context->Stop.load(std::memory_order_relaxed)) {
        AK_EVENT event_record{};
        AK_STATUS status;

        status = AkWaitEvent(context->Session, kEventWaitPollMs, &event_record);
        if (status == AK_STATUS_TIMEOUT) {
            continue;
        }
        if (status == AK_STATUS_NO_MORE_ENTRIES) {
            continue;
        }
        if (status != AK_STATUS_SUCCESS) {
            LogWide(context, L"event loop failed, status=" + FormatStatusHex(status));
            context->Stop.store(true, std::memory_order_relaxed);
            if (context->StopEvent != nullptr) {
                SetEvent(context->StopEvent);
            }
            break;
        }

        HandleAppKernelEvent(context, &event_record);
    }
}

void RunCommandLoop(
    BackendContext* context)
{
    PrintRuntimeHelp();

    while (!context->Stop.load(std::memory_order_relaxed)) {
        std::cout << "> " << std::flush;

        std::string line;
        std::istringstream input;
        std::string command;

        if (!std::getline(std::cin, line)) {
            if (context->StopEvent != nullptr) {
                SetEvent(context->StopEvent);
            }
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
            if (context->StopEvent != nullptr) {
                SetEvent(context->StopEvent);
            }
            break;
        }
        if (command == "ct") {
            CreateDiskRequest request{};
            std::string token;
            std::vector<std::string> tokens;
            std::wstring error_text;

            while (input >> token) {
                tokens.push_back(token);
            }

            if (!ParseCreateDiskCommand(context->Config, tokens, &request, &error_text)) {
                std::wcerr << L"create failed, reason=" << error_text << std::endl;
                continue;
            }

            if (request.TargetId >= YUMEDISK_MAX_TARGETS) {
                request.TargetId = FindFirstFreeTarget(context);
                if (request.TargetId >= YUMEDISK_MAX_TARGETS) {
                    std::wcerr << L"no free target" << std::endl;
                    continue;
                }
            }

            (void)CreateManagedDisk(context, request);
            continue;
        }
        if (command == "rm") {
            std::string arg;
            ULONG target_id;

            if (!(input >> arg)) {
                std::wcerr << L"rm requires <target>|all" << std::endl;
                continue;
            }

            if (arg == "all") {
                (void)RemoveAllManagedDisks(context, false);
                continue;
            }

            target_id = 0;
            if (!ParseTargetToken(arg, &target_id)) {
                std::wcerr << L"invalid target: " << std::wstring(arg.begin(), arg.end()) << std::endl;
                continue;
            }

            (void)RemoveManagedDisk(context, target_id);
            continue;
        }

        std::wcerr << L"unknown command: " << std::wstring(command.begin(), command.end()) << std::endl;
    }
}

bool RemoveAllManagedDisks(
    BackendContext* context,
    bool closing)
{
    const auto disks = SnapshotManagedDisks(context);
    bool ok;

    ok = true;
    for (const auto& disk : disks) {
        if (disk == nullptr) {
            continue;
        }

        if (disk->Handle != nullptr) {
            if (AkRemoveDisk(disk->Handle) != AK_STATUS_SUCCESS) {
                if (!closing) {
                    std::wcerr << L"remove all failed, target=" << disk->TargetId << std::endl;
                }
                ok = false;
                continue;
            }
            disk->Handle = nullptr;
        }

        CleanupManagedDiskMedia(disk.get());
    }

    {
        std::lock_guard<std::mutex> guard(context->DisksLock);
        context->Disks.clear();
    }

    std::wcout << L"removed_all=true" << std::endl;
    return ok || closing;
}

} // namespace testapp
