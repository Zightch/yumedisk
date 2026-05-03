#include "session.h"

static
UINT64
ControlGenerateSessionId(
    VOID
)
{
    LARGE_INTEGER tick;

    KeQuerySystemTimePrecise(&tick);
    return ((UINT64)tick.QuadPart) ^ (UINT64)(ULONG_PTR)PsGetCurrentProcessId();
}

VOID
ControlSessionInitialize(
    _Out_ PCTRL_DEVICE_CONTEXT Context
)
{
    Context->OpenCount = 0;
    Context->OpenFileObject = NULL;
    Context->SessionId = 0;
}

NTSTATUS
ControlSessionTryOpen(
    _Inout_ PCTRL_DEVICE_CONTEXT Context,
    _In_ WDFFILEOBJECT FileObject,
    _Out_opt_ UINT64* SessionId
)
{
    NTSTATUS status;
    UINT64 sessionId;

    status = STATUS_SUCCESS;
    sessionId = 0;

    WdfSpinLockAcquire(Context->OpenLock);
    if (Context->OpenCount != 0) {
        status = STATUS_SHARING_VIOLATION;
    } else {
        Context->OpenCount = 1;
        Context->OpenFileObject = FileObject;
        Context->SessionId = ControlGenerateSessionId();
        sessionId = Context->SessionId;
    }
    WdfSpinLockRelease(Context->OpenLock);

    if (SessionId != NULL) {
        *SessionId = sessionId;
    }

    return status;
}

UINT64
ControlSessionClose(
    _Inout_ PCTRL_DEVICE_CONTEXT Context,
    _In_ WDFFILEOBJECT FileObject
)
{
    UINT64 sessionId;

    sessionId = 0;

    WdfSpinLockAcquire(Context->OpenLock);
    if (Context->OpenCount != 0 && Context->OpenFileObject == FileObject) {
        sessionId = Context->SessionId;
        Context->OpenCount = 0;
        Context->OpenFileObject = NULL;
        Context->SessionId = 0;
    }
    WdfSpinLockRelease(Context->OpenLock);

    return sessionId;
}

UINT64
ControlSessionGetActiveId(
    _In_ PCTRL_DEVICE_CONTEXT Context
)
{
    UINT64 sessionId;

    sessionId = 0;

    WdfSpinLockAcquire(Context->OpenLock);
    if (Context->OpenCount == 1 && Context->OpenFileObject != NULL) {
        sessionId = Context->SessionId;
    }
    WdfSpinLockRelease(Context->OpenLock);

    return sessionId;
}

