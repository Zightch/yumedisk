#pragma once

#include "..\core\defs.h"

typedef struct _CTRL_ASYNC_SLOT_REQUEST {
    LIST_ENTRY PoolLink;
    LIST_ENTRY SubmitLink;
    PCTRL_TRANSPORT_RUNTIME Runtime;
    PCTRL_FILE_CONTEXT SessionContext;
    WDFREQUEST Request;
    PIRP Irp;
    UINT64 SlotId;
    UINT32 TargetId;
    UINT32 SlotType;
    PUCHAR DirectBuffer;
    size_t DirectBufferSize;
    PUCHAR IoctlBuffer;
    ULONG IoctlBufferSize;
    IO_STATUS_BLOCK IoStatusBlock;
    volatile LONG CompletionState;
    NTSTATUS CompletionStatus;
    ULONG_PTR CompletionInformation;
    CCHAR IrpStackSize;
    BOOLEAN IrpHasBeenSubmitted;
} CTRL_ASYNC_SLOT_REQUEST, *PCTRL_ASYNC_SLOT_REQUEST;

NTSTATUS
ControlTransportRuntimeStart(
    _Inout_ PCTRL_FILE_CONTEXT Context,
    _In_ WDFFILEOBJECT FileObject
);

VOID
ControlTransportRuntimeBeginClose(
    _Inout_ PCTRL_FILE_CONTEXT Context
);

VOID
ControlTransportRuntimeStop(
    _Inout_ PCTRL_FILE_CONTEXT Context
);

NTSTATUS
ControlTransportRuntimeAcquireSlotRequest(
    _Inout_ PCTRL_FILE_CONTEXT Context,
    _In_ CCHAR IrpStackSize,
    _In_ ULONG IoctlBufferSize,
    _Outptr_ PCTRL_ASYNC_SLOT_REQUEST* SlotRequest
);

VOID
ControlTransportRuntimeReleaseSlotRequest(
    _Inout_opt_ PCTRL_ASYNC_SLOT_REQUEST SlotRequest
);

NTSTATUS
ControlTransportRuntimeSubmitSlotRequest(
    _Inout_ PCTRL_ASYNC_SLOT_REQUEST SlotRequest
);
