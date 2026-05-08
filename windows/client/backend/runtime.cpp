#include "runtime.h"

#include <algorithm>
#include <iomanip>
#include <sstream>

#include "config.h"
#include "media.h"
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

void appendLog(
    BackendContext* context,
    const std::wstring& text)
{
    std::wstring line;

    if (context == nullptr) {
        return;
    }

    {
        std::lock_guard<std::mutex> guard(context->logLock);
        if (context->logLines.size() >= maxBufferedLogLines) {
            context->logLines.erase(context->logLines.begin());
        }
        context->logLines.push_back(text);
    }

    line = text + L"\n";
    OutputDebugStringW(line.c_str());
}

const wchar_t* readOnlyToText(
    bool readOnly)
{
    return readOnly ? L"true" : L"false";
}

std::vector<std::shared_ptr<ManagedDisk>> snapshotManagedDisks(
    BackendContext* context)
{
    std::vector<std::shared_ptr<ManagedDisk>> disks;

    if (context == nullptr) {
        return disks;
    }

    std::lock_guard<std::mutex> guard(context->disksLock);
    disks.reserve(context->disks.size());
    for (const auto& entry : context->disks) {
        disks.push_back(entry.second);
    }

    return disks;
}

std::shared_ptr<ManagedDisk> findManagedDisk(
    BackendContext* context,
    ULONG targetId)
{
    std::lock_guard<std::mutex> guard(context->disksLock);
    const auto it = context->disks.find(targetId);
    if (it == context->disks.end()) {
        return nullptr;
    }

    return it->second;
}

void insertManagedDisk(
    BackendContext* context,
    const std::shared_ptr<ManagedDisk>& disk)
{
    std::lock_guard<std::mutex> guard(context->disksLock);
    context->disks[disk->targetId] = disk;
}

void eraseManagedDisk(
    BackendContext* context,
    ULONG targetId)
{
    std::lock_guard<std::mutex> guard(context->disksLock);
    context->disks.erase(targetId);
}

bool managedDiskExists(
    BackendContext* context,
    ULONG targetId)
{
    std::lock_guard<std::mutex> guard(context->disksLock);
    return context->disks.find(targetId) != context->disks.end();
}

std::vector<std::wstring> snapshotClaimedDiskPaths(
    BackendContext* context,
    ULONG excludedTargetId)
{
    std::vector<std::wstring> paths;

    std::lock_guard<std::mutex> guard(context->disksLock);
    for (const auto& entry : context->disks) {
        if (entry.first == excludedTargetId) {
            continue;
        }
        if (!entry.second->identity.Path.empty()) {
            paths.push_back(entry.second->identity.Path);
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

bool tryRefreshManagedDiskIdentity(
    BackendContext* context,
    const std::shared_ptr<ManagedDisk>& disk,
    const std::vector<DiskIdentity>* baselineVisibleDisks,
    DWORD timeoutMs)
{
    const auto claimedPaths = snapshotClaimedDiskPaths(context, disk->targetId);
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
            disk->identity = *selected;
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

void handleAppKernelEvent(
    BackendContext* context,
    const AK_EVENT* eventRecord)
{
    std::shared_ptr<ManagedDisk> disk;

    if ((context == nullptr) || (eventRecord == nullptr)) {
        return;
    }

    if (eventRecord->Type == AkEventSessionBroken) {
        appendLog(
            context,
            L"[backend] session broken, status=" + formatStatusHex(eventRecord->Status));
        context->stop.store(true, std::memory_order_relaxed);
        if (context->stopEvent != nullptr) {
            SetEvent(context->stopEvent);
        }
        return;
    }

    disk = findManagedDisk(context, eventRecord->TargetId);
    if (disk == nullptr) {
        return;
    }

    switch (eventRecord->Type) {
    case AkEventDiskOnline:
        (void)tryRefreshManagedDiskIdentity(context, disk, nullptr, 0);
        break;

    case AkEventWriteFinalCommitted:
        if (!applyCommittedWrite(disk.get(), eventRecord->EventId)) {
            appendLog(
                context,
                L"[backend] commit write failed, target=" + std::to_wstring(eventRecord->TargetId) +
                    L", event=" + std::to_wstring(eventRecord->EventId));
        }
        break;

    case AkEventWriteFinalRejected:
        discardStagedWrite(disk.get(), eventRecord->EventId);
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
            appendLog(context, L"[backend] event loop failed, status=" + formatStatusHex(status));
            context->stop.store(true, std::memory_order_relaxed);
            if (context->stopEvent != nullptr) {
                SetEvent(context->stopEvent);
            }
            break;
        }

        handleAppKernelEvent(context, &eventRecord);
    }
}

void discardAllManagedDiskState(
    BackendContext* context)
{
    const auto disks = snapshotManagedDisks(context);

    for (const auto& disk : disks) {
        if (disk == nullptr) {
            continue;
        }

        disk->handle = nullptr;
        cleanupManagedDiskMedia(disk.get());
    }

    {
        std::lock_guard<std::mutex> guard(context->disksLock);
        context->disks.clear();
    }
}

} // namespace

bool openBackendContext(BackendContext* context) {
    AK_OPEN_PARAMS openParams{};
    AK_SESSION_STATE sessionState{};
    AK_STATUS status;

    if (context == nullptr) {
        return false;
    }

    context->stop.store(false, std::memory_order_relaxed);
    context->openStatus = AK_STATUS_SUCCESS;
    context->openWin32Error = ERROR_SUCCESS;
    context->openSucceeded = false;

    context->stopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (context->stopEvent == nullptr) {
        context->openStatus = AK_STATUS_UNSUCCESSFUL;
        context->openWin32Error = GetLastError();
        appendLog(
            context,
            L"[backend] create stop event failed, win32=" + std::to_wstring(context->openWin32Error));
        return false;
    }

    openParams.HeartbeatIntervalMs = heartbeatIntervalMs;
    openParams.InitialEventQueueCapacity = initialEventQueueCapacity;
    openParams.LogFn = appKernelLogCallback;
    openParams.LogCtx = context;

    status = AkOpen(&openParams, &context->session);
    if (status != AK_STATUS_SUCCESS) {
        context->openStatus = status;
        context->openWin32Error = GetLastError();
        appendLog(
            context,
            L"[backend] open session failed, status=" + formatStatusHex(status) +
                L", win32=" + std::to_wstring(context->openWin32Error));
        CloseHandle(context->stopEvent);
        context->stopEvent = nullptr;
        return false;
    }

    context->openSucceeded = true;
    if (AkQuerySessionState(context->session, &sessionState) == AK_STATUS_SUCCESS) {
        appendLog(
            context,
            L"[backend] session opened, id=" + std::to_wstring(sessionState.SessionId) +
                L", lifecycle=" + lifecycleToText(sessionState.Lifecycle) +
                L", transport=" + std::wstring(sessionState.TransportReady ? L"ready" : L"not-ready"));
        appendLog(
            context,
            L"[backend] appkernel=" + formatVersionBe(sessionState.AppKernelVersionBe) +
                L", kmdf=" + formatVersionBe(sessionState.KmdfVersionBe) +
                L", scsi=" + formatVersionBe(sessionState.ScsiVersionBe));
    } else {
        appendLog(context, L"[backend] session opened");
    }
    appendLog(
        context,
        L"[backend] config queueDepth=" + std::to_wstring(context->config.queueDepth) +
            L", writeSlotBytes=" + std::to_wstring(context->config.writeSlotBytes) +
            L", sectorSize=" + std::to_wstring(context->config.sectorSize));

    try {
        context->eventThread = std::thread(runEventLoop, context);
    } catch (const std::exception&) {
        context->openStatus = AK_STATUS_UNSUCCESSFUL;
        context->openWin32Error = ERROR_NOT_ENOUGH_MEMORY;
        appendLog(context, L"[backend] start event thread failed");
        AkClose(context->session);
        context->session = nullptr;
        CloseHandle(context->stopEvent);
        context->stopEvent = nullptr;
        context->openSucceeded = false;
        return false;
    }

    return true;
}

void closeBackendContext(BackendContext* context) {
    if (context == nullptr) {
        return;
    }

    (void)removeAllManagedDisks(context, true);

    context->stop.store(true, std::memory_order_relaxed);
    if (context->stopEvent != nullptr) {
        SetEvent(context->stopEvent);
    }
    if (context->eventThread.joinable()) {
        context->eventThread.join();
    }

    if (context->session != nullptr) {
        appendLog(context, L"[backend] closing session");
        AkClose(context->session);
        context->session = nullptr;
    }

    discardAllManagedDiskState(context);

    if (context->stopEvent != nullptr) {
        CloseHandle(context->stopEvent);
        context->stopEvent = nullptr;
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
    appendLog(
        context,
        L"[AppKernel][" + std::to_wstring(level) + L"] " + wideText);
}

std::wstring querySessionStateText(
    const BackendContext* context)
{
    AK_SESSION_STATE sessionState{};
    AK_STATUS status;
    std::wostringstream stream;

    if (context == nullptr) {
        return L"backend-missing";
    }

    if (context->session == nullptr) {
        if (!context->openSucceeded) {
            stream << L"open-failed("
                   << formatStatusHex(context->openStatus)
                   << L")";
            return stream.str();
        }

        return L"closed";
    }

    status = AkQuerySessionState(context->session, &sessionState);
    if (status != AK_STATUS_SUCCESS) {
        return L"query-failed(" + formatStatusHex(status) + L")";
    }

    stream << L"session=" << sessionState.SessionId
           << L", lifecycle=" << lifecycleToText(sessionState.Lifecycle)
           << L", transport=" << (sessionState.TransportReady ? L"ready" : L"not-ready")
           << L", disks=" << sessionState.DiskCount;
    return stream.str();
}

std::vector<std::wstring> snapshotLogLines(
    const BackendContext* context)
{
    std::vector<std::wstring> lines;

    if (context == nullptr) {
        return lines;
    }

    std::lock_guard<std::mutex> guard(context->logLock);
    lines = context->logLines;
    return lines;
}

ULONG findFirstFreeTarget(
    BackendContext* context)
{
    std::lock_guard<std::mutex> guard(context->disksLock);

    for (ULONG targetId = YUMEDISK_MIN_TARGET_ID;
         targetId <= YUMEDISK_MAX_USABLE_TARGET_ID;
         ++targetId) {
        if (context->disks.find(targetId) == context->disks.end()) {
            return targetId;
        }
    }

    return YUMEDISK_MAX_TARGETS;
}

bool createManagedDisk(
    BackendContext* context,
    const CreateDiskRequest& request,
    std::wstring* outErrorText)
{
    std::shared_ptr<ManagedDisk> disk;
    AK_DISK_PARAMS params{};
    AK_DISK* handle;
    AK_STATUS status;
    std::wstring mediaReason;
    const auto visibleDisksBeforeCreate = EnumerateVisibleYumeDisks();
    ULONG targetId = request.targetId;

    if ((context == nullptr) || (context->session == nullptr)) {
        if (outErrorText != nullptr) {
            *outErrorText = L"session-not-open";
        }
        return false;
    }

    if (targetId >= YUMEDISK_MAX_TARGETS) {
        targetId = findFirstFreeTarget(context);
        if (targetId >= YUMEDISK_MAX_TARGETS) {
            if (outErrorText != nullptr) {
                *outErrorText = L"no-free-target";
            }
            return false;
        }
    }

    if (managedDiskExists(context, targetId)) {
        if (outErrorText != nullptr) {
            *outErrorText = L"target-already-exists";
        }
        return false;
    }

    disk = std::make_shared<ManagedDisk>();
    disk->backend = context;
    disk->targetId = targetId;
    disk->sectorSize = context->config.sectorSize;
    disk->diskSizeBytes = request.diskSizeBytes;
    disk->readOnly = request.readOnly;
    disk->slotDepth = context->config.queueDepth;
    disk->readWorkerCount = computeWorkerCount(
        disk->slotDepth,
        readSlotsPerWorkerTarget,
        maxReadWorkersPerDisk);
    disk->writeWorkerCount = computeWorkerCount(
        disk->slotDepth,
        writeSlotsPerWorkerTarget,
        maxWriteWorkersPerDisk);

    if (!initializeManagedDiskMedia(disk.get(), request.requestedMode, &mediaReason)) {
        if (outErrorText != nullptr) {
            *outErrorText = mediaReason;
        }
        appendLog(
            context,
            L"[backend] create failed, target=" + std::to_wstring(targetId) +
                L", reason=" + mediaReason);
        return false;
    }

    insertManagedDisk(context, disk);

    params.TargetId = targetId;
    params.SectorSize = context->config.sectorSize;
    params.DiskSizeBytes = request.diskSizeBytes;
    params.QueueDepth = (UINT32)context->config.queueDepth;
    params.WriteSlotBytes = (UINT32)context->config.writeSlotBytes;
    params.ReadWorkerCount = (UINT16)disk->readWorkerCount;
    params.WriteWorkerCount = (UINT16)disk->writeWorkerCount;
    params.AckBatchMaxRanges = (UINT32)context->config.queueDepth;
    params.ReadOnly = request.readOnly ? 1u : 0u;

    handle = nullptr;
    status = AkCreateDisk(context->session, &params, &mediaOps, disk.get(), &handle);
    if (status != AK_STATUS_SUCCESS) {
        eraseManagedDisk(context, targetId);
        cleanupManagedDiskMedia(disk.get());
        if (outErrorText != nullptr) {
            *outErrorText = formatStatusHex(status);
        }
        appendLog(
            context,
            L"[backend] create failed, target=" + std::to_wstring(targetId) +
                L", status=" + formatStatusHex(status));
        return false;
    }

    disk->handle = handle;
    appendLog(
        context,
        L"[backend] created target=" + std::to_wstring(targetId) +
            L", diskBytes=" + std::to_wstring(request.diskSizeBytes) +
            L", readOnly=" + readOnlyToText(disk->readOnly) +
            L", media=" + mediaModeToText(disk->mode));

    if (tryRefreshManagedDiskIdentity(context, disk, &visibleDisksBeforeCreate, diskArrivalTimeoutMs)) {
        appendLog(
            context,
            L"[backend] visiblePath=" + disk->identity.Path +
                L", physicalDrive=" + MakePhysicalDrivePath(disk->identity.DeviceNumber));
    } else {
        appendLog(
            context,
            L"[backend] visiblePath=<pending-enumeration>, target=" + std::to_wstring(targetId));
    }

    return true;
}

bool removeManagedDisk(
    BackendContext* context,
    ULONG targetId,
    std::wstring* outErrorText)
{
    const auto disk = findManagedDisk(context, targetId);
    AK_STATUS status;

    if ((context == nullptr) || (context->session == nullptr)) {
        if (outErrorText != nullptr) {
            *outErrorText = L"session-not-open";
        }
        return false;
    }

    if (disk == nullptr) {
        if (outErrorText != nullptr) {
            *outErrorText = L"target-not-found";
        }
        return false;
    }

    if (disk->handle == nullptr) {
        cleanupManagedDiskMedia(disk.get());
        eraseManagedDisk(context, targetId);
        appendLog(context, L"[backend] removed target=" + std::to_wstring(targetId));
        return true;
    }

    status = AkRemoveDisk(disk->handle);
    if (status != AK_STATUS_SUCCESS) {
        if (outErrorText != nullptr) {
            *outErrorText = formatStatusHex(status);
        }
        appendLog(
            context,
            L"[backend] remove failed, target=" + std::to_wstring(targetId) +
                L", status=" + formatStatusHex(status));
        return false;
    }

    disk->handle = nullptr;
    cleanupManagedDiskMedia(disk.get());
    eraseManagedDisk(context, targetId);
    appendLog(context, L"[backend] removed target=" + std::to_wstring(targetId));
    return true;
}

bool removeAllManagedDisks(
    BackendContext* context,
    bool closing)
{
    const auto disks = snapshotManagedDisks(context);
    std::vector<ULONG> removedTargetIds;
    bool ok;

    if (context == nullptr) {
        return false;
    }

    ok = true;
    for (const auto& disk : disks) {
        if (disk == nullptr) {
            continue;
        }

        if ((disk->handle != nullptr) && (context->session != nullptr)) {
            if (AkRemoveDisk(disk->handle) != AK_STATUS_SUCCESS) {
                appendLog(
                    context,
                    L"[backend] remove all failed, target=" + std::to_wstring(disk->targetId));
                ok = false;
                if (!closing) {
                    continue;
                }
            } else {
                disk->handle = nullptr;
            }
        }

        cleanupManagedDiskMedia(disk.get());
        removedTargetIds.push_back(disk->targetId);
    }

    {
        std::lock_guard<std::mutex> guard(context->disksLock);
        for (ULONG targetId : removedTargetIds) {
            context->disks.erase(targetId);
        }
    }

    appendLog(context, L"[backend] removedAll=true");
    return ok || closing;
}

} // namespace clientbackend
