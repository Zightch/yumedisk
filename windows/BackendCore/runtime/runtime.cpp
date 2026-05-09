#include "runtime/runtime.h"

#include <algorithm>
#include <atomic>
#include <exception>
#include <iomanip>
#include <map>
#include <memory>
#include <mutex>
#include <sstream>
#include <thread>

#include "config/config.h"
#include "media/media.h"
#include "runtime/runtimeDisk.h"
#include "scan.h"

namespace clientbackend {

using yumedisk::scan::EnumerateVisibleYumeDisks;
using yumedisk::scan::MakePhysicalDrivePath;

VOID AK_CALL appKernelLogCallback(
    void* logCtx,
    INT level,
    const char* text);

struct BackendContext::Impl {
    SessionConfig sessionConfig;
    AK_SESSION* session = nullptr;
    HANDLE stopEvent = nullptr;
    std::atomic<bool> stop{false};
    std::mutex diskRuntimesLock;
    std::map<ULONG, std::shared_ptr<DiskRuntime>> diskRuntimes;
    mutable std::mutex logLock;
    std::vector<std::wstring> logLines;
    std::thread eventThread;
    AK_STATUS openStatus = AK_STATUS_SUCCESS;
    DWORD openWin32Error = ERROR_SUCCESS;
    bool openSucceeded = false;
};

struct RuntimeAccess {
    static BackendContext::Impl& state(
        BackendContext* context)
    {
        return *context->impl;
    }

    static const BackendContext::Impl& state(
        const BackendContext* context)
    {
        return *context->impl;
    }
};

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

    auto& state = RuntimeAccess::state(context);

    std::lock_guard<std::mutex> guard(state.diskRuntimesLock);
    diskRuntimes.reserve(state.diskRuntimes.size());
    for (const auto& entry : state.diskRuntimes) {
        diskRuntimes.push_back(entry.second);
    }

    return diskRuntimes;
}

std::shared_ptr<DiskRuntime> findDiskRuntime(
    BackendContext* context,
    ULONG targetId)
{
    auto& state = RuntimeAccess::state(context);

    std::lock_guard<std::mutex> guard(state.diskRuntimesLock);
    const auto it = state.diskRuntimes.find(targetId);
    if (it == state.diskRuntimes.end()) {
        return nullptr;
    }

    return it->second;
}

void insertDiskRuntime(
    BackendContext* context,
    const std::shared_ptr<DiskRuntime>& diskRuntime)
{
    auto& state = RuntimeAccess::state(context);

    std::lock_guard<std::mutex> guard(state.diskRuntimesLock);
    state.diskRuntimes[diskRuntime->metadata.targetId] = diskRuntime;
}

void eraseDiskRuntime(
    BackendContext* context,
    ULONG targetId)
{
    auto& state = RuntimeAccess::state(context);

    std::lock_guard<std::mutex> guard(state.diskRuntimesLock);
    state.diskRuntimes.erase(targetId);
}

bool diskRuntimeExists(
    BackendContext* context,
    ULONG targetId)
{
    auto& state = RuntimeAccess::state(context);

    std::lock_guard<std::mutex> guard(state.diskRuntimesLock);
    return state.diskRuntimes.find(targetId) != state.diskRuntimes.end();
}

std::vector<std::wstring> snapshotClaimedDiskPaths(
    BackendContext* context,
    ULONG excludedTargetId)
{
    std::vector<std::wstring> paths;
    auto& state = RuntimeAccess::state(context);

    std::lock_guard<std::mutex> guard(state.diskRuntimesLock);
    for (const auto& entry : state.diskRuntimes) {
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
            RuntimeAccess::state(context).stop.load(std::memory_order_relaxed) ||
            ((GetTickCount64() - startTick) >= timeoutMs)) {
            return false;
        }

        Sleep(diskArrivalPollMs);
    }
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
    snapshot.mediaKind = diskRuntime->metadata.mediaKind;

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
        auto& state = RuntimeAccess::state(context);

        context->appendLog(
            L"[backend] session broken, status=" + formatStatusHex(eventRecord->Status));
        state.stop.store(true, std::memory_order_relaxed);
        if (state.stopEvent != nullptr) {
            SetEvent(state.stopEvent);
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
    auto& state = RuntimeAccess::state(context);

    while (!state.stop.load(std::memory_order_relaxed)) {
        AK_EVENT eventRecord{};
        AK_STATUS status;

        status = AkWaitEvent(state.session, eventWaitPollMs, &eventRecord);
        if ((status == AK_STATUS_TIMEOUT) || (status == AK_STATUS_NO_MORE_ENTRIES)) {
            continue;
        }
        if (status != AK_STATUS_SUCCESS) {
            context->appendLog(L"[backend] event loop failed, status=" + formatStatusHex(status));
            state.stop.store(true, std::memory_order_relaxed);
            if (state.stopEvent != nullptr) {
                SetEvent(state.stopEvent);
            }
            break;
        }

        handleAppKernelEvent(context, &eventRecord);
    }
}

void discardAllDiskRuntimeState(
    BackendContext* context)
{
    if (context == nullptr) {
        return;
    }

    const auto diskRuntimes = snapshotDiskRuntimes(context);

    for (const auto& diskRuntime : diskRuntimes) {
        if (diskRuntime == nullptr) {
            continue;
        }

        diskRuntime->lifecycle.handle = nullptr;
        cleanupManagedDiskMedia(diskRuntime.get());
    }

    {
        auto& state = RuntimeAccess::state(context);
        std::lock_guard<std::mutex> guard(state.diskRuntimesLock);
        state.diskRuntimes.clear();
    }
}

} // namespace

BackendContext::BackendContext()
    : impl(std::make_unique<Impl>())
{
}

BackendContext::~BackendContext()
{
    close();
}

void BackendContext::setSessionConfig(
    const SessionConfig& sessionConfigValue)
{
    auto& state = RuntimeAccess::state(this);

    if (state.session != nullptr) {
        appendLog(L"[backend] ignore session config update while open");
        return;
    }

    state.sessionConfig = sessionConfigValue;
}

SessionConfig BackendContext::sessionConfig() const
{
    return RuntimeAccess::state(this).sessionConfig;
}

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
    AK_SESSION_STATE sessionState{};
    AK_STATUS status;
    std::wstring errorText;
    auto& state = RuntimeAccess::state(this);

    if (state.session != nullptr) {
        return true;
    }

    state.stop.store(false, std::memory_order_relaxed);
    state.openStatus = AK_STATUS_SUCCESS;
    state.openWin32Error = ERROR_SUCCESS;
    state.openSucceeded = false;

    if (!validateSessionConfig(state.sessionConfig, &errorText)) {
        state.openStatus = AK_STATUS_INVALID_PARAMETER;
        state.openWin32Error = ERROR_INVALID_PARAMETER;
        appendLog(L"[backend] invalid session config, reason=" + errorText);
        return false;
    }

    state.stopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (state.stopEvent == nullptr) {
        state.openStatus = AK_STATUS_UNSUCCESSFUL;
        state.openWin32Error = GetLastError();
        appendLog(L"[backend] create stop event failed, win32=" + std::to_wstring(state.openWin32Error));
        return false;
    }

    const AK_OPEN_PARAMS openParams = buildAkOpenParams(
        state.sessionConfig,
        appKernelLogCallback,
        this);
    status = AkOpen(&openParams, &state.session);
    if (status != AK_STATUS_SUCCESS) {
        state.openStatus = status;
        state.openWin32Error = GetLastError();
        appendLog(
            L"[backend] open session failed, status=" + formatStatusHex(status) +
            L", win32=" + std::to_wstring(state.openWin32Error));
        CloseHandle(state.stopEvent);
        state.stopEvent = nullptr;
        return false;
    }

    state.openSucceeded = true;
    if (AkQuerySessionState(state.session, &sessionState) == AK_STATUS_SUCCESS) {
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
        L"[backend] sessionConfig heartbeatIntervalMs=" + std::to_wstring(state.sessionConfig.heartbeatIntervalMs) +
        L", initialEventQueueCapacity=" + std::to_wstring(state.sessionConfig.initialEventQueueCapacity));

    try {
        state.eventThread = std::thread(runEventLoop, this);
    } catch (const std::exception&) {
        state.openStatus = AK_STATUS_UNSUCCESSFUL;
        state.openWin32Error = ERROR_NOT_ENOUGH_MEMORY;
        appendLog(L"[backend] start event thread failed");
        AkClose(state.session);
        state.session = nullptr;
        CloseHandle(state.stopEvent);
        state.stopEvent = nullptr;
        state.openSucceeded = false;
        return false;
    }

    return true;
}

void BackendContext::close()
{
    auto& state = RuntimeAccess::state(this);

    (void)removeAllManagedDisks(true);

    state.stop.store(true, std::memory_order_relaxed);
    if (state.stopEvent != nullptr) {
        SetEvent(state.stopEvent);
    }
    if (state.eventThread.joinable()) {
        state.eventThread.join();
    }

    if (state.session != nullptr) {
        appendLog(L"[backend] closing session");
        AkClose(state.session);
        state.session = nullptr;
    }

    discardAllDiskRuntimeState(this);

    if (state.stopEvent != nullptr) {
        CloseHandle(state.stopEvent);
        state.stopEvent = nullptr;
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
    if (context == nullptr) {
        return;
    }

    wideText = wideFromText(text);
    context->appendLog(
        L"[AppKernel][" + std::to_wstring(level) + L"] " + wideText);
}

void BackendContext::appendLog(
    const std::wstring& text)
{
    std::wstring line;
    auto& state = RuntimeAccess::state(this);

    {
        std::lock_guard<std::mutex> guard(state.logLock);
        if (state.logLines.size() >= maxBufferedLogLines) {
            state.logLines.erase(state.logLines.begin());
        }
        state.logLines.push_back(text);
    }

    line = text + L"\n";
    OutputDebugStringW(line.c_str());
}

std::wstring BackendContext::querySessionStateText() const
{
    AK_SESSION_STATE sessionState{};
    AK_STATUS status;
    std::wostringstream stream;
    const auto& state = RuntimeAccess::state(this);

    if (state.session == nullptr) {
        if (!state.openSucceeded) {
            stream << L"open-failed("
                   << formatStatusHex(state.openStatus)
                   << L")";
            return stream.str();
        }

        return L"closed";
    }

    status = AkQuerySessionState(state.session, &sessionState);
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
    const auto& state = RuntimeAccess::state(this);

    std::lock_guard<std::mutex> guard(state.logLock);
    lines = state.logLines;
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
    auto* mutableContext = const_cast<BackendContext*>(this);
    const auto& state = RuntimeAccess::state(this);

    if (outStats == nullptr) {
        if (outErrorText != nullptr) {
            *outErrorText = L"invalid-parameter";
        }
        return false;
    }

    if (state.session == nullptr) {
        if (outErrorText != nullptr) {
            *outErrorText = L"session-not-open";
        }
        return false;
    }

    status = AkQuerySessionStats(state.session, &sessionStats);
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
    outStats->diskCount = (UINT64)snapshotDiskRuntimes(mutableContext).size();
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
    auto& state = RuntimeAccess::state(this);

    std::lock_guard<std::mutex> guard(state.diskRuntimesLock);

    for (ULONG targetId = YUMEDISK_MIN_TARGET_ID;
         targetId <= YUMEDISK_MAX_USABLE_TARGET_ID;
         ++targetId) {
        if (state.diskRuntimes.find(targetId) == state.diskRuntimes.end()) {
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
    DiskConfig diskConfig = request.diskConfig;
    AK_DISK* handle;
    AK_STATUS status;
    std::wstring configError;
    std::wstring mediaReason;
    const auto visibleDisksBeforeCreate = EnumerateVisibleYumeDisks();
    auto& state = RuntimeAccess::state(this);

    if (state.session == nullptr) {
        if (outErrorText != nullptr) {
            *outErrorText = L"session-not-open";
        }
        return false;
    }

    if (diskConfig.targetId == YUMEDISK_MAX_TARGETS) {
        diskConfig.targetId = findFirstFreeTarget();
        if (diskConfig.targetId >= YUMEDISK_MAX_TARGETS) {
            if (outErrorText != nullptr) {
                *outErrorText = L"no-free-target";
            }
            return false;
        }
    }

    if (!validateCreateDiskRequest(
            CreateDiskRequest{diskConfig, request.mediaKind},
            &configError)) {
        if (outErrorText != nullptr) {
            *outErrorText = configError;
        }
        return false;
    }

    if (diskRuntimeExists(this, diskConfig.targetId)) {
        if (outErrorText != nullptr) {
            *outErrorText = L"target-already-exists";
        }
        return false;
    }

    diskRuntime = std::make_shared<DiskRuntime>();
    diskRuntime->context = this;
    diskRuntime->metadata.targetId = diskConfig.targetId;
    diskRuntime->metadata.sectorSize = diskConfig.sectorSize;
    diskRuntime->metadata.diskSizeBytes = diskConfig.diskSizeBytes;
    diskRuntime->metadata.readOnly = diskConfig.readOnly;
    diskRuntime->metadata.mediaKind = request.mediaKind;
    diskRuntime->queueConfig.queueDepth = diskConfig.queueDepth;
    diskRuntime->queueConfig.writeSlotBytes = diskConfig.writeSlotBytes;
    diskRuntime->queueConfig.readWorkerCount = diskConfig.readWorkerCount;
    diskRuntime->queueConfig.writeWorkerCount = diskConfig.writeWorkerCount;
    diskRuntime->queueConfig.ackBatchMaxRanges = diskConfig.ackBatchMaxRanges;

    if (!initializeManagedDiskMedia(diskRuntime.get(), request.mediaKind, &mediaReason)) {
        if (outErrorText != nullptr) {
            *outErrorText = mediaReason;
        }
        appendLog(
            L"[backend] create failed, target=" + std::to_wstring(diskConfig.targetId) +
            L", reason=" + mediaReason);
        return false;
    }

    insertDiskRuntime(this, diskRuntime);

    diskConfig.diskSizeBytes = diskRuntime->metadata.diskSizeBytes;
    const AK_DISK_PARAMS params = buildAkDiskParams(diskConfig);
    handle = nullptr;
    status = AkCreateDisk(state.session, &params, &mediaOps, diskRuntime.get(), &handle);
    if (status != AK_STATUS_SUCCESS) {
        eraseDiskRuntime(this, diskConfig.targetId);
        cleanupManagedDiskMedia(diskRuntime.get());
        if (outErrorText != nullptr) {
            *outErrorText = formatStatusHex(status);
        }
        appendLog(
            L"[backend] create failed, target=" + std::to_wstring(diskConfig.targetId) +
            L", status=" + formatStatusHex(status));
        return false;
    }

    diskRuntime->lifecycle.handle = handle;
    appendLog(
        L"[backend] created target=" + std::to_wstring(diskConfig.targetId) +
        L", diskBytes=" + std::to_wstring(diskRuntime->metadata.diskSizeBytes) +
        L", readOnly=" + readOnlyToText(diskRuntime->metadata.readOnly) +
        L", media=" + mediaKindToText(diskRuntime->metadata.mediaKind) +
        L", queueDepth=" + std::to_wstring(diskRuntime->queueConfig.queueDepth) +
        L", writeSlotBytes=" + std::to_wstring(diskRuntime->queueConfig.writeSlotBytes) +
        L", readWorkerCount=" + std::to_wstring(diskRuntime->queueConfig.readWorkerCount) +
        L", writeWorkerCount=" + std::to_wstring(diskRuntime->queueConfig.writeWorkerCount) +
        L", ackBatchMaxRanges=" + std::to_wstring(diskRuntime->queueConfig.ackBatchMaxRanges));

    if (tryRefreshDiskRuntimeIdentity(this, diskRuntime, &visibleDisksBeforeCreate, diskArrivalTimeoutMs)) {
        appendLog(
            L"[backend] visiblePath=" + diskRuntime->metadata.identity.Path +
            L", physicalDrive=" + MakePhysicalDrivePath(diskRuntime->metadata.identity.DeviceNumber));
    } else {
        appendLog(
            L"[backend] visiblePath=<pending-enumeration>, target=" + std::to_wstring(diskConfig.targetId));
    }

    return true;
}

bool BackendContext::removeManagedDisk(
    ULONG targetId,
    std::wstring* outErrorText)
{
    const auto diskRuntime = findDiskRuntime(this, targetId);
    AK_STATUS status;
    const auto& state = RuntimeAccess::state(this);

    if (state.session == nullptr) {
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
    auto& state = RuntimeAccess::state(this);

    for (const auto& diskRuntime : runtimeList) {
        if (diskRuntime == nullptr) {
            continue;
        }

        if ((diskRuntime->lifecycle.handle != nullptr) && (state.session != nullptr)) {
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
        std::lock_guard<std::mutex> guard(state.diskRuntimesLock);
        for (ULONG targetId : removedTargetIds) {
            state.diskRuntimes.erase(targetId);
        }
    }

    appendLog(L"[backend] removedAll=true");
    return ok || closing;
}

} // namespace clientbackend
