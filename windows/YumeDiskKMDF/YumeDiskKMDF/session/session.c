#include "session.h"

#include "..\transport\transport.h"

#define CTRL_SESSION_WATCHDOG_TIMEOUT_MS 10000u
#define CTRL_SESSION_LOCKED_STATUS STATUS_FILE_LOCK_CONFLICT

EVT_WDF_TIMER ControlEvtSessionWatchdogTimer;

static
LONGLONG
ControlSessionQueryTick(
    VOID
)
{
    LARGE_INTEGER tick;

    KeQuerySystemTimePrecise(&tick);
    return tick.QuadPart;
}

static
UINT64
ControlGenerateSessionId(
    VOID
)
{
    return ((UINT64)ControlSessionQueryTick()) ^ (UINT64)(ULONG_PTR)PsGetCurrentProcessId();
}

static
NTSTATUS
ControlSessionCreateResources(
    _Inout_ PCTRL_FILE_CONTEXT FileContext,
    _In_ WDFFILEOBJECT FileObject
)
{
    WDF_OBJECT_ATTRIBUTES attributes;
    WDF_TIMER_CONFIG timerConfig;
    NTSTATUS status;

    if (FileContext->SessionLock == NULL) {
        WDF_OBJECT_ATTRIBUTES_INIT(&attributes);
        attributes.ParentObject = FileObject;
        status = WdfWaitLockCreate(&attributes, &FileContext->SessionLock);
        if (!NT_SUCCESS(status)) {
            return status;
        }

        KeInitializeEvent(&FileContext->InFlightZeroEvent, NotificationEvent, TRUE);
        FileContext->InFlightRequestCount = 0;
    }

    if (FileContext->WatchdogTimer == NULL) {
        WDF_TIMER_CONFIG_INIT(&timerConfig, ControlEvtSessionWatchdogTimer);
        timerConfig.AutomaticSerialization = FALSE;

        WDF_OBJECT_ATTRIBUTES_INIT(&attributes);
        attributes.ParentObject = FileObject;
        attributes.ExecutionLevel = WdfExecutionLevelPassive;
        status = WdfTimerCreate(&timerConfig, &attributes, &FileContext->WatchdogTimer);
        if (!NT_SUCCESS(status)) {
            return status;
        }
    }

    return STATUS_SUCCESS;
}

static
NTSTATUS
ControlSessionEnterLocked(
    _In_ PCTRL_FILE_CONTEXT FileContext,
    _Out_opt_ UINT64* SessionId
)
{
    if (FileContext->State == CtrlSessionStateLocked) {
        return CTRL_SESSION_LOCKED_STATUS;
    }

    if (FileContext->State != CtrlSessionStateActive ||
        FileContext->MiniportHandle == NULL ||
        FileContext->SessionId == 0) {
        return STATUS_DEVICE_NOT_READY;
    }

    if (SessionId != NULL) {
        *SessionId = FileContext->SessionId;
    }

    return STATUS_SUCCESS;
}

static
VOID
ControlSessionReferenceIoLocked(
    _Inout_ PCTRL_FILE_CONTEXT FileContext
)
{
    if (InterlockedIncrement(&FileContext->InFlightRequestCount) == 1) {
        KeClearEvent(&FileContext->InFlightZeroEvent);
    }
}

static
VOID
ControlSessionDrainInFlightIo(
    _Inout_ PCTRL_FILE_CONTEXT FileContext
)
{
    while (InterlockedCompareExchange(&FileContext->InFlightRequestCount, 0, 0) != 0) {
        KeWaitForSingleObject(&FileContext->InFlightZeroEvent, Executive, KernelMode, FALSE, NULL);
    }
}

static
VOID
ControlSessionReleaseDeviceOpen(
    _Inout_ PCTRL_DEVICE_CONTEXT Context,
    _In_ WDFFILEOBJECT FileObject
)
{
    WdfSpinLockAcquire(Context->OpenLock);
    if (Context->OpenCount != 0 && Context->OpenFileObject == FileObject) {
        Context->OpenCount = 0;
        Context->OpenFileObject = NULL;
    }
    WdfSpinLockRelease(Context->OpenLock);
}

VOID
ControlEvtSessionWatchdogTimer(
    _In_ WDFTIMER Timer
)
{
    WDFFILEOBJECT fileObject;
    PCTRL_FILE_CONTEXT fileContext;
    LONGLONG nowTick;
    LONGLONG elapsedTick;

    fileObject = (WDFFILEOBJECT)WdfTimerGetParentObject(Timer);
    fileContext = ControlGetFileContext(fileObject);
    if (fileContext->SessionLock == NULL) {
        return;
    }

    WdfWaitLockAcquire(fileContext->SessionLock, NULL);
    if (fileContext->State != CtrlSessionStateActive ||
        fileContext->MiniportHandle == NULL ||
        fileContext->SessionId == 0) {
        WdfWaitLockRelease(fileContext->SessionLock);
        return;
    }

    nowTick = ControlSessionQueryTick();
    elapsedTick = nowTick - fileContext->LastHeartbeatTick;
    if (elapsedTick < ((LONGLONG)CTRL_SESSION_WATCHDOG_TIMEOUT_MS * 10000ll)) {
        WdfWaitLockRelease(fileContext->SessionLock);
        return;
    }

    fileContext->State = CtrlSessionStateLocked;
    WdfWaitLockRelease(fileContext->SessionLock);

    ControlSendSessionCleanup(fileContext);
    ControlSessionDrainInFlightIo(fileContext);
    ControlCloseMiniportHandle(fileContext);

    WdfTimerStop(Timer, FALSE);
}

VOID
ControlSessionInitialize(
    _Out_ PCTRL_DEVICE_CONTEXT Context
)
{
    Context->OpenCount = 0;
    Context->OpenFileObject = NULL;
}

NTSTATUS
ControlSessionTryOpen(
    _Inout_ PCTRL_DEVICE_CONTEXT Context,
    _In_ WDFFILEOBJECT FileObject,
    _Out_opt_ UINT64* SessionId
)
{
    NTSTATUS status;
    PCTRL_FILE_CONTEXT fileContext;
    HANDLE handle;
    UINT64 sessionId;

    fileContext = ControlGetFileContext(FileObject);
    handle = NULL;
    sessionId = 0;

    WdfSpinLockAcquire(Context->OpenLock);
    if (Context->OpenCount != 0) {
        WdfSpinLockRelease(Context->OpenLock);
        if (SessionId != NULL) {
            *SessionId = 0;
        }
        return STATUS_SHARING_VIOLATION;
    }

    Context->OpenCount = 1;
    Context->OpenFileObject = FileObject;
    WdfSpinLockRelease(Context->OpenLock);

    status = ControlSessionCreateResources(fileContext, FileObject);
    if (!NT_SUCCESS(status)) {
        ControlSessionReleaseDeviceOpen(Context, FileObject);
        if (SessionId != NULL) {
            *SessionId = 0;
        }
        return status;
    }

    status = ControlOpenMiniportHandle(&handle);
    if (!NT_SUCCESS(status)) {
        ControlSessionReleaseDeviceOpen(Context, FileObject);
        if (SessionId != NULL) {
            *SessionId = 0;
        }
        return status;
    }

    sessionId = ControlGenerateSessionId();

    WdfWaitLockAcquire(fileContext->SessionLock, NULL);
    fileContext->MiniportHandle = handle;
    fileContext->SessionId = sessionId;
    fileContext->LastHeartbeatTick = ControlSessionQueryTick();
    fileContext->State = CtrlSessionStateActive;
    WdfWaitLockRelease(fileContext->SessionLock);

    WdfTimerStart(fileContext->WatchdogTimer, WDF_REL_TIMEOUT_IN_MS(CTRL_SESSION_WATCHDOG_TIMEOUT_MS));

    if (SessionId != NULL) {
        *SessionId = sessionId;
    }

    return STATUS_SUCCESS;
}

VOID
ControlSessionCleanup(
    _Inout_ PCTRL_DEVICE_CONTEXT Context,
    _In_ WDFFILEOBJECT FileObject
)
{
    PCTRL_FILE_CONTEXT fileContext;

    fileContext = ControlGetFileContext(FileObject);

    if (fileContext->WatchdogTimer != NULL) {
        WdfTimerStop(fileContext->WatchdogTimer, TRUE);
    }

    if (fileContext->SessionLock != NULL) {
        WdfWaitLockAcquire(fileContext->SessionLock, NULL);
        fileContext->State = CtrlSessionStateClosed;
        WdfWaitLockRelease(fileContext->SessionLock);

        ControlSendSessionCleanup(fileContext);
        ControlSessionDrainInFlightIo(fileContext);

        WdfWaitLockAcquire(fileContext->SessionLock, NULL);
        ControlCloseMiniportHandle(fileContext);
        fileContext->SessionId = 0;
        fileContext->LastHeartbeatTick = 0;
        WdfWaitLockRelease(fileContext->SessionLock);
    }

    ControlSessionReleaseDeviceOpen(Context, FileObject);
}

NTSTATUS
ControlSessionHeartbeat(
    _In_ WDFFILEOBJECT FileObject,
    _Out_opt_ UINT64* SessionId
)
{
    NTSTATUS status;
    PCTRL_FILE_CONTEXT fileContext;

    fileContext = ControlGetFileContext(FileObject);
    if (fileContext->SessionLock == NULL) {
        if (SessionId != NULL) {
            *SessionId = 0;
        }
        return STATUS_DEVICE_NOT_READY;
    }

    WdfWaitLockAcquire(fileContext->SessionLock, NULL);
    status = ControlSessionEnterLocked(fileContext, SessionId);
    if (NT_SUCCESS(status)) {
        fileContext->LastHeartbeatTick = ControlSessionQueryTick();
        if (fileContext->WatchdogTimer != NULL) {
            WdfTimerStop(fileContext->WatchdogTimer, FALSE);
            WdfTimerStart(fileContext->WatchdogTimer, WDF_REL_TIMEOUT_IN_MS(CTRL_SESSION_WATCHDOG_TIMEOUT_MS));
        }
    }
    WdfWaitLockRelease(fileContext->SessionLock);
    return status;
}

NTSTATUS
ControlSessionAcquire(
    _In_ WDFFILEOBJECT FileObject,
    _Outptr_ PCTRL_FILE_CONTEXT* SessionContext,
    _Out_opt_ UINT64* SessionId
)
{
    NTSTATUS status;
    PCTRL_FILE_CONTEXT fileContext;

    *SessionContext = NULL;
    if (SessionId != NULL) {
        *SessionId = 0;
    }

    if (FileObject == NULL) {
        return STATUS_INVALID_DEVICE_REQUEST;
    }

    fileContext = ControlGetFileContext(FileObject);
    if (fileContext->SessionLock == NULL) {
        return STATUS_DEVICE_NOT_READY;
    }

    WdfWaitLockAcquire(fileContext->SessionLock, NULL);
    status = ControlSessionEnterLocked(fileContext, SessionId);
    if (!NT_SUCCESS(status)) {
        WdfWaitLockRelease(fileContext->SessionLock);
        return status;
    }

    ControlSessionReferenceIoLocked(fileContext);
    *SessionContext = fileContext;
    WdfWaitLockRelease(fileContext->SessionLock);
    return STATUS_SUCCESS;
}

VOID
ControlSessionRelease(
    _In_ PCTRL_FILE_CONTEXT SessionContext
)
{
    if (SessionContext == NULL) {
        return;
    }

    if (InterlockedDecrement(&SessionContext->InFlightRequestCount) == 0) {
        KeSetEvent(&SessionContext->InFlightZeroEvent, IO_NO_INCREMENT, FALSE);
    }
}
