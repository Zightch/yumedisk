#include "backend/runtime/runtime.h"

#include <algorithm>
#include <iomanip>
#include <sstream>

#include "backend/config/config.h"
#include "backend/media/media.h"
#include "scan.h"

namespace clientbackend {

using yumedisk::scan::EnumerateVisibleYumeDisks;
using yumedisk::scan::MakePhysicalDrivePath;

VOID AK_CALL appKernelLogCallback(
    void* logCtx,
    INT level,
    const char* text);

namespace {

const AK_MEDIA_OPS mediaOps = {
    hostReadBytes,
    hostStageWrite
};

std::wstring wideFromMultiByte(
    const char* text,
    UINT codePage)
{
    int length;
    std::wstring result;

    if ((text == nullptr) || (*text == '\0')) {
        return {};
    }

    length = MultiByteToWideChar(codePage, 0, text, -1, nullptr, 0);
    if (length <= 1) {
        return {};
    }

    result.resize((size_t)length);
    (void)MultiByteToWideChar(codePage, 0, text, -1, result.data(), length);
    if (!result.empty() && result.back() == L'\0') {
        result.pop_back();
    }

    return result;
}

std::wstring wideFromText(
    const char* text)
{
    std::wstring result = wideFromMultiByte(text, CP_UTF8);

    if (!result.empty()) {
        return result;
    }

    return wideFromMultiByte(text, CP_ACP);
}

const wchar_t* readOnlyToText(
    bool readOnly)
{
    return readOnly ? L"true" : L"false";
}

std::vector<std::shared_ptr<DiskRuntime>> snapshotDiskRuntimes(
    BackendContext* context)
{
    std::vector<std::shared_ptr<DiskRuntime>> diskRuntimes;

    if (context == nullptr) {
        return diskRuntimes;
    }

    std::lock_guard<std::mutex> guard(context->diskRuntimesLock);
    diskRuntimes.reserve(context->diskRuntimes.size());
    for (const auto& entry : context->diskRuntimes) {
        diskRuntimes.push_back(entry.second);
    }

    return diskRuntimes;
}

std::shared_ptr<DiskRuntime> findDiskRuntime(
    BackendContext* context,
    ULONG targetId)
{
    std::lock_guard<std::mutex> guard(context->diskRuntimesLock);
    const auto it = context->diskRuntimes.find(targetId);
    if (it == context->diskRuntimes.end()) {
        return nullptr;
    }

    return it->second;
}

void insertDiskRuntime(
    BackendContext* context,
    const std::shared_ptr<DiskRuntime>& diskRuntime)
{
    std::lock_guard<std::mutex> guard(context->diskRuntimesLock);
    context->diskRuntimes[diskRuntime->metadata.targetId] = diskRuntime;
}

void eraseDiskRuntime(
    BackendContext* context,
    ULONG targetId)
{
    std::lock_guard<std::mutex> guard(context->diskRuntimesLock);
    context->diskRuntimes.erase(targetId);
}

bool diskRuntimeExists(
    BackendContext* context,
    ULONG targetId)
{
    std::lock_guard<std::mutex> guard(context->diskRuntimesLock);
    return context->diskRuntimes.find(targetId) != context->diskRuntimes.end();
}

std::vector<std::wstring> snapshotClaimedDiskPaths(
    BackendContext* context,
    ULONG excludedTargetId)
{
    std::vector<std::wstring> paths;

    std::lock_guard<std::mutex> guard(context->diskRuntimesLock);
    for (const auto& entry : context->diskRuntimes) {
        if (entry.first == excludedTargetId) {
            continue;
        }
        if (!entry.second->metadata.identity.Path.empty()) {
            paths.push_back(entry.second->metadata.identity.Path);
        }
    }

    return paths;
}

bool containsPath(
    const std::vector<std::wstring>& paths,
    const std::wstring& path)
{
    return std::find(paths.begin(), paths.end(), path) != paths.end();
}

bool containsVisibleDiskPath(
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

bool tryRefreshDiskRuntimeIdentity(
    BackendContext* context,
    const std::shared_ptr<DiskRuntime>& diskRuntime,
    const std::vector<DiskIdentity>* baselineVisibleDisks,
    DWORD timeoutMs)
{
    const auto claimedPaths = snapshotClaimedDiskPaths(context, diskRuntime->metadata.targetId);
    const ULONGLONG startTick = GetTickCount64();

    for (;;) {
        const auto visibleDisks = EnumerateVisibleYumeDisks();
        const DiskIdentity* selected = nullptr;

        for (const auto& identity : visibleDisks) {
            if (containsPath(claimedPaths, identity.Path)) {
                continue;
            }
            if ((baselineVisibleDisks != nullptr) &&
                containsVisibleDiskPath(*baselineVisibleDisks, identity.Path)) {
                continue;
            }

            selected = &identity;
            break;
        }

        if ((selected == nullptr) && !visibleDisks.empty()) {
            for (const auto& identity : visibleDisks) {
                if (!containsPath(claimedPaths, identity.Path)) {
                    selected = &identity;
                    break;
                }
            }
        }

        if (selected != nullptr) {
            diskRuntime->metadata.identity = *selected;
            return true;
        }

        if ((timeoutMs == 0) ||
            context->stop.load(std::memory_order_relaxed) ||
            ((GetTickCount64() - startTick) >= timeoutMs)) {
            return false;
        }

        Sleep(diskArrivalPollMs);
    }
}

size_t computeWorkerCount(
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

std::wstring lifecycleToText(
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

ManagedDiskSnapshot makeManagedDiskSnapshot(
    BackendContext* context,
    const std::shared_ptr<DiskRuntime>& diskRuntime)
{
    ManagedDiskSnapshot snapshot;
    AK_DISK_STATE diskState{};
    bool haveState;

    snapshot.targetId = diskRuntime->metadata.targetId;
    snapshot.diskSizeBytes = diskRuntime->metadata.diskSizeBytes;
    snapshot.sectorSize = diskRuntime->metadata.sectorSize;
    snapshot.readOnly = diskRuntime->metadata.readOnly;
    snapshot.mode = diskRuntime->metadata.mode;

    (void)tryRefreshDiskRuntimeIdentity(context, diskRuntime, nullptr, 0);
    snapshot.visiblePath = diskRuntime->metadata.identity.Path;
    snapshot.physicalDrivePath = MakePhysicalDrivePath(diskRuntime->metadata.identity.DeviceNumber);

    haveState = (diskRuntime->lifecycle.handle != nullptr) &&
        (AkQueryDiskState(diskRuntime->lifecycle.handle, &diskState) == AK_STATUS_SUCCESS);
    snapshot.online = haveState && (diskState.Lifecycle == AkStateRunning);
    snapshot.lifecycleText = haveState
        ? lifecycleToText(diskState.Lifecycle)
        : L"unknown";

    return snapshot;
}

void handleAppKernelEvent(
    BackendContext* context,
    const AK_EVENT* eventRecord)
{
    std::shared_ptr<DiskRuntime> diskRuntime;

    if ((context == nullptr) || (eventRecord == nullptr)) {
        return;
    }

    if (eventRecord->Type == AkEventSessionBroken) {
        context->appendLog(
            L"[backend] session broken, status=" + formatStatusHex(eventRecord->Status));
        context->stop.store(true, std::memory_order_relaxed);
        if (context->stopEvent != nullptr) {
            SetEvent(context->stopEvent);
        }
        return;
    }

    diskRuntime = findDiskRuntime(context, eventRecord->TargetId);
    if (diskRuntime == nullptr) {
        return;
    }

    switch (eventRecord->Type) {
    case AkEventDiskOnline:
        (void)tryRefreshDiskRuntimeIdentity(context, diskRuntime, nullptr, 0);
        break;

    case AkEventWriteFinalCommitted:
        if (!commitDiskRuntimeStaging(diskRuntime.get(), eventRecord->EventId)) {
            context->appendLog(
                L"[backend] commit write failed, target=" + std::to_wstring(eventRecord->TargetId) +
                L", event=" + std::to_wstring(eventRecord->EventId));
        }
        break;

    case AkEventWriteFinalRejected:
        rejectDiskRuntimeStaging(diskRuntime.get(), eventRecord->EventId);
        break;

    case AkEventDiskRemoved:
    default:
        break;
    }
}

void runEventLoop(
    BackendContext* context)
{
    while (!context->stop.load(std::memory_order_relaxed)) {
        AK_EVENT eventRecord{};
        AK_STATUS status;

        status = AkWaitEvent(context->session, eventWaitPollMs, &eventRecord);
        if ((status == AK_STATUS_TIMEOUT) || (status == AK_STATUS_NO_MORE_ENTRIES)) {
            continue;
        }
        if (status != AK_STATUS_SUCCESS) {
            context->appendLog(L"[backend] event loop failed, status=" + formatStatusHex(status));
            context->stop.store(true, std::memory_order_relaxed);
            if (context->stopEvent != nullptr) {
                SetEvent(context->stopEvent);
            }
            break;
        }

        handleAppKernelEvent(context, &eventRecord);
    }
}

void discardAllDiskRuntimeState(
    BackendContext* context)
{
    const auto diskRuntimes = snapshotDiskRuntimes(context);

    for (const auto& diskRuntime : diskRuntimes) {
        if (diskRuntime == nullptr) {
            continue;
        }

        diskRuntime->lifecycle.handle = nullptr;
        cleanupManagedDiskMedia(diskRuntime.get());
    }

    {
        std::lock_guard<std::mutex> guard(context->diskRuntimesLock);
        context->diskRuntimes.clear();
    }
}

} // namespace

bool openBackendContext(BackendContext* context)
{
    return context != nullptr && context->open();
}

void closeBackendContext(BackendContext* context)
{
    if (context != nullptr) {
        context->close();
    }
}

bool BackendContext::open()
{
    AK_OPEN_PARAMS openParams{};
    AK_SESSION_STATE sessionState{};
    AK_STATUS status;

    stop.store(false, std::memory_order_relaxed);
    openStatus = AK_STATUS_SUCCESS;
    openWin32Error = ERROR_SUCCESS;
    openSucceeded = false;

    stopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (stopEvent == nullptr) {
        openStatus = AK_STATUS_UNSUCCESSFUL;
        openWin32Error = GetLastError();
        appendLog(L"[backend] create stop event failed, win32=" + std::to_wstring(openWin32Error));
        return false;
    }

    openParams.HeartbeatIntervalMs = heartbeatIntervalMs;
    openParams.InitialEventQueueCapacity = initialEventQueueCapacity;
    openParams.LogFn = appKernelLogCallback;
    openParams.LogCtx = this;

    status = AkOpen(&openParams, &session);
    if (status != AK_STATUS_SUCCESS) {
        openStatus = status;
        openWin32Error = GetLastError();
        appendLog(
            L"[backend] open session failed, status=" + formatStatusHex(status) +
            L", win32=" + std::to_wstring(openWin32Error));
        CloseHandle(stopEvent);
        stopEvent = nullptr;
        return false;
    }

    openSucceeded = true;
    if (AkQuerySessionState(session, &sessionState) == AK_STATUS_SUCCESS) {
        appendLog(
            L"[backend] session opened, id=" + std::to_wstring(sessionState.SessionId) +
            L", lifecycle=" + lifecycleToText(sessionState.Lifecycle) +
            L", transport=" + std::wstring(sessionState.TransportReady ? L"ready" : L"not-ready"));
        appendLog(
            L"[backend] appkernel=" + formatVersionBe(sessionState.AppKernelVersionBe) +
            L", kmdf=" + formatVersionBe(sessionState.KmdfVersionBe) +
            L", scsi=" + formatVersionBe(sessionState.ScsiVersionBe));
    } else {
        appendLog(L"[backend] session opened");
    }
    appendLog(
        L"[backend] config queueDepth=" + std::to_wstring(config.queueDepth) +
        L", writeSlotBytes=" + std::to_wstring(config.writeSlotBytes) +
        L", sectorSize=" + std::to_wstring(config.sectorSize));

    try {
        eventThread = std::thread(runEventLoop, this);
    } catch (const std::exception&) {
        openStatus = AK_STATUS_UNSUCCESSFUL;
        openWin32Error = ERROR_NOT_ENOUGH_MEMORY;
        appendLog(L"[backend] start event thread failed");
        AkClose(session);
        session = nullptr;
        CloseHandle(stopEvent);
        stopEvent = nullptr;
        openSucceeded = false;
        return false;
    }

    return true;
}

void BackendContext::close()
{
    (void)removeAllManagedDisks(true);

    stop.store(true, std::memory_order_relaxed);
    if (stopEvent != nullptr) {
        SetEvent(stopEvent);
    }
    if (eventThread.joinable()) {
        eventThread.join();
    }

    if (session != nullptr) {
        appendLog(L"[backend] closing session");
        AkClose(session);
        session = nullptr;
    }

    discardAllDiskRuntimeState(this);

    if (stopEvent != nullptr) {
        CloseHandle(stopEvent);
        stopEvent = nullptr;
    }
}

std::wstring formatStatusHex(
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

std::wstring formatVersionBe(
    UINT32 versionBe)
{
    std::wostringstream stream;

    stream << ((versionBe >> 24) & 0xffu)
           << L'.' << ((versionBe >> 16) & 0xffu)
           << L'.' << ((versionBe >> 8) & 0xffu)
           << L'.' << (versionBe & 0xffu);
    return stream.str();
}

VOID AK_CALL appKernelLogCallback(
    void* logCtx,
    INT level,
    const char* text)
{
    BackendContext* context;
    std::wstring wideText;

    context = static_cast<BackendContext*>(logCtx);
    wideText = wideFromText(text);
    context->appendLog(
        L"[AppKernel][" + std::to_wstring(level) + L"] " + wideText);
}

void BackendContext::appendLog(
    const std::wstring& text)
{
    std::wstring line;

    {
        std::lock_guard<std::mutex> guard(logLock);
        if (logLines.size() >= maxBufferedLogLines) {
            logLines.erase(logLines.begin());
        }
        logLines.push_back(text);
    }

    line = text + L"\n";
    OutputDebugStringW(line.c_str());
}

std::wstring BackendContext::querySessionStateText() const
{
    AK_SESSION_STATE sessionState{};
    AK_STATUS status;
    std::wostringstream stream;

    if (session == nullptr) {
        if (!openSucceeded) {
            stream << L"open-failed("
                   << formatStatusHex(openStatus)
                   << L")";
            return stream.str();
        }

        return L"closed";
    }

    status = AkQuerySessionState(session, &sessionState);
    if (status != AK_STATUS_SUCCESS) {
        return L"query-failed(" + formatStatusHex(status) + L")";
    }

    stream << L"session=" << sessionState.SessionId
           << L", lifecycle=" << lifecycleToText(sessionState.Lifecycle)
           << L", transport=" << (sessionState.TransportReady ? L"ready" : L"not-ready")
           << L", disks=" << sessionState.DiskCount;
    return stream.str();
}

std::vector<std::wstring> BackendContext::snapshotLogLines() const
{
    std::vector<std::wstring> lines;

    std::lock_guard<std::mutex> guard(logLock);
    lines = logLines;
    return lines;
}

std::vector<ManagedDiskSnapshot> BackendContext::snapshotManagedDisks() const
{
    std::vector<ManagedDiskSnapshot> snapshots;
    auto* mutableContext = const_cast<BackendContext*>(this);

    for (const auto& diskRuntime : snapshotDiskRuntimes(mutableContext)) {
        if (diskRuntime == nullptr) {
            continue;
        }
        snapshots.push_back(makeManagedDiskSnapshot(mutableContext, diskRuntime));
    }

    return snapshots;
}

bool BackendContext::queryBackendStats(
    BackendStatsSnapshot* outStats,
    std::wstring* outErrorText) const
{
    AK_SESSION_STATS sessionStats{};
    AK_STATUS status;

    if (outStats == nullptr) {
        if (outErrorText != nullptr) {
            *outErrorText = L"invalid-parameter";
        }
        return false;
    }

    if (session == nullptr) {
        if (outErrorText != nullptr) {
            *outErrorText = L"session-not-open";
        }
        return false;
    }

    status = AkQuerySessionStats(session, &sessionStats);
    if (status != AK_STATUS_SUCCESS) {
        if (outErrorText != nullptr) {
            *outErrorText = formatStatusHex(status);
        }
        return false;
    }

    outStats->heartbeatSent = sessionStats.HeartbeatSent;
    outStats->commandFailures = sessionStats.CommandFailures;
    outStats->protocolFailures = sessionStats.ProtocolFailures;
    outStats->eventsQueued = sessionStats.EventsQueued;
    outStats->eventsDropped = sessionStats.EventsDropped;
    outStats->diskCount = (UINT64)snapshotManagedDisks().size();
    return true;
}

bool BackendContext::queryDebugSnapshot(
    DebugSnapshot* outSnapshot,
    std::wstring* outErrorText) const
{
    if (outSnapshot == nullptr) {
        if (outErrorText != nullptr) {
            *outErrorText = L"invalid-parameter";
        }
        return false;
    }

    outSnapshot->sessionStateText = querySessionStateText();
    outSnapshot->disks = snapshotManagedDisks();
    if (!queryBackendStats(&outSnapshot->stats, outErrorText)) {
        return false;
    }

    return true;
}

ULONG BackendContext::findFirstFreeTarget()
{
    std::lock_guard<std::mutex> guard(diskRuntimesLock);

    for (ULONG targetId = YUMEDISK_MIN_TARGET_ID;
         targetId <= YUMEDISK_MAX_USABLE_TARGET_ID;
         ++targetId) {
        if (diskRuntimes.find(targetId) == diskRuntimes.end()) {
            return targetId;
        }
    }

    return YUMEDISK_MAX_TARGETS;
}

bool BackendContext::createManagedDisk(
    const CreateDiskRequest& request,
    std::wstring* outErrorText)
{
    std::shared_ptr<DiskRuntime> diskRuntime;
    AK_DISK_PARAMS params{};
    AK_DISK* handle;
    AK_STATUS status;
    std::wstring mediaReason;
    const auto visibleDisksBeforeCreate = EnumerateVisibleYumeDisks();
    ULONG targetId = request.targetId;

    if (session == nullptr) {
        if (outErrorText != nullptr) {
            *outErrorText = L"session-not-open";
        }
        return false;
    }

    if (targetId >= YUMEDISK_MAX_TARGETS) {
        targetId = findFirstFreeTarget();
        if (targetId >= YUMEDISK_MAX_TARGETS) {
            if (outErrorText != nullptr) {
                *outErrorText = L"no-free-target";
            }
            return false;
        }
    }

    if (diskRuntimeExists(this, targetId)) {
        if (outErrorText != nullptr) {
            *outErrorText = L"target-already-exists";
        }
        return false;
    }

    diskRuntime = std::make_shared<DiskRuntime>();
    diskRuntime->context = this;
    diskRuntime->metadata.targetId = targetId;
    diskRuntime->metadata.sectorSize = config.sectorSize;
    diskRuntime->metadata.diskSizeBytes = request.diskSizeBytes;
    diskRuntime->metadata.readOnly = request.readOnly;
    diskRuntime->metadata.backingFilePath = request.rawFilePath;
    diskRuntime->queueConfig.slotDepth = config.queueDepth;
    diskRuntime->queueConfig.readWorkerCount = computeWorkerCount(
        diskRuntime->queueConfig.slotDepth,
        readSlotsPerWorkerTarget,
        maxReadWorkersPerDisk);
    diskRuntime->queueConfig.writeWorkerCount = computeWorkerCount(
        diskRuntime->queueConfig.slotDepth,
        writeSlotsPerWorkerTarget,
        maxWriteWorkersPerDisk);

    if (!initializeManagedDiskMedia(diskRuntime.get(), request.requestedMode, &mediaReason)) {
        if (outErrorText != nullptr) {
            *outErrorText = mediaReason;
        }
        appendLog(
            L"[backend] create failed, target=" + std::to_wstring(targetId) +
            L", reason=" + mediaReason);
        return false;
    }

    insertDiskRuntime(this, diskRuntime);

    params.TargetId = targetId;
    params.SectorSize = config.sectorSize;
    params.DiskSizeBytes = diskRuntime->metadata.diskSizeBytes;
    params.QueueDepth = (UINT32)config.queueDepth;
    params.WriteSlotBytes = (UINT32)config.writeSlotBytes;
    params.ReadWorkerCount = (UINT16)diskRuntime->queueConfig.readWorkerCount;
    params.WriteWorkerCount = (UINT16)diskRuntime->queueConfig.writeWorkerCount;
    params.AckBatchMaxRanges = (UINT32)config.queueDepth;
    params.ReadOnly = request.readOnly ? 1u : 0u;

    handle = nullptr;
    status = AkCreateDisk(session, &params, &mediaOps, diskRuntime.get(), &handle);
    if (status != AK_STATUS_SUCCESS) {
        eraseDiskRuntime(this, targetId);
        cleanupManagedDiskMedia(diskRuntime.get());
        if (outErrorText != nullptr) {
            *outErrorText = formatStatusHex(status);
        }
        appendLog(
            L"[backend] create failed, target=" + std::to_wstring(targetId) +
            L", status=" + formatStatusHex(status));
        return false;
    }

    diskRuntime->lifecycle.handle = handle;
    appendLog(
        L"[backend] created target=" + std::to_wstring(targetId) +
        L", diskBytes=" + std::to_wstring(diskRuntime->metadata.diskSizeBytes) +
        L", readOnly=" + readOnlyToText(diskRuntime->metadata.readOnly) +
        L", media=" + mediaModeToText(diskRuntime->metadata.mode));
    if (diskRuntime->metadata.mode == MediaMode::rawFile) {
        appendLog(L"[backend] rawFile=" + diskRuntime->metadata.backingFilePath);
    }

    if (tryRefreshDiskRuntimeIdentity(this, diskRuntime, &visibleDisksBeforeCreate, diskArrivalTimeoutMs)) {
        appendLog(
            L"[backend] visiblePath=" + diskRuntime->metadata.identity.Path +
            L", physicalDrive=" + MakePhysicalDrivePath(diskRuntime->metadata.identity.DeviceNumber));
    } else {
        appendLog(
            L"[backend] visiblePath=<pending-enumeration>, target=" + std::to_wstring(targetId));
    }

    return true;
}

bool BackendContext::removeManagedDisk(
    ULONG targetId,
    std::wstring* outErrorText)
{
    const auto diskRuntime = findDiskRuntime(this, targetId);
    AK_STATUS status;

    if (session == nullptr) {
        if (outErrorText != nullptr) {
            *outErrorText = L"session-not-open";
        }
        return false;
    }

    if (diskRuntime == nullptr) {
        if (outErrorText != nullptr) {
            *outErrorText = L"target-not-found";
        }
        return false;
    }

    if (diskRuntime->lifecycle.handle == nullptr) {
        cleanupManagedDiskMedia(diskRuntime.get());
        eraseDiskRuntime(this, targetId);
        appendLog(L"[backend] removed target=" + std::to_wstring(targetId));
        return true;
    }

    status = AkRemoveDisk(diskRuntime->lifecycle.handle);
    if (status != AK_STATUS_SUCCESS) {
        if (outErrorText != nullptr) {
            *outErrorText = formatStatusHex(status);
        }
        appendLog(
            L"[backend] remove failed, target=" + std::to_wstring(targetId) +
            L", status=" + formatStatusHex(status));
        return false;
    }

    diskRuntime->lifecycle.handle = nullptr;
    cleanupManagedDiskMedia(diskRuntime.get());
    eraseDiskRuntime(this, targetId);
    appendLog(L"[backend] removed target=" + std::to_wstring(targetId));
    return true;
}

bool BackendContext::removeAllManagedDisks(bool closing)
{
    const auto runtimeList = snapshotDiskRuntimes(this);
    std::vector<ULONG> removedTargetIds;
    bool ok = true;

    for (const auto& diskRuntime : runtimeList) {
        if (diskRuntime == nullptr) {
            continue;
        }

        if ((diskRuntime->lifecycle.handle != nullptr) && (session != nullptr)) {
            if (AkRemoveDisk(diskRuntime->lifecycle.handle) != AK_STATUS_SUCCESS) {
                appendLog(
                    L"[backend] remove all failed, target=" + std::to_wstring(diskRuntime->metadata.targetId));
                ok = false;
                if (!closing) {
                    continue;
                }
            } else {
                diskRuntime->lifecycle.handle = nullptr;
            }
        }

        cleanupManagedDiskMedia(diskRuntime.get());
        removedTargetIds.push_back(diskRuntime->metadata.targetId);
    }

    {
        std::lock_guard<std::mutex> guard(diskRuntimesLock);
        for (ULONG targetId : removedTargetIds) {
            diskRuntimes.erase(targetId);
        }
    }

    appendLog(L"[backend] removedAll=true");
    return ok || closing;
}

} // namespace clientbackend
