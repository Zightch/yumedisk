#include "transport.h"

#include <ntddscsi.h>

#include "..\core\memory.h"

static const GUID ControlStoragePortGuid = {
    0x2accfe60, 0xc130, 0x11d2, { 0xb0, 0x82, 0x00, 0xa0, 0xc9, 0x1e, 0xfb, 0x8b }
};
static const WCHAR ControlMiniportServiceName[] = L"YumeDiskSCSI";

static
BOOLEAN
ControlMessageHeaderLooksValid(
    _In_ const YUMEDISK_MESSAGE* Message,
    _In_ ULONG ExpectedCommand,
    _In_ ULONG BufferCapacity
)
{
    if (Message->Header.Version != YUMEDISK_PROTOCOL_VERSION ||
        Message->Header.Command != ExpectedCommand ||
        Message->Header.Size < (ULONG)YUMEDISK_MESSAGE_BASE_SIZE ||
        Message->Header.Size > BufferCapacity ||
        Message->Header.PayloadLength > (Message->Header.Size - (ULONG)YUMEDISK_MESSAGE_BASE_SIZE)) {
        return FALSE;
    }

    return TRUE;
}

static
BOOLEAN
ControlTryRecoverFailedQueryInfo(
    _In_ PSRB_IO_CONTROL SrbIoControl,
    _In_ ULONG BufferCapacity,
    _Out_ ULONG* EffectiveInformation
)
{
    const PYUMEDISK_MESSAGE message = (PYUMEDISK_MESSAGE)(SrbIoControl + 1);
    const PYUMEDISK_QUERY_INFO info = (PYUMEDISK_QUERY_INFO)message->Payload;
    SIZE_T signatureMatch;
    SIZE_T serviceNameMatch;

    if (!ControlMessageHeaderLooksValid(message, YumeDiskCommandQueryInfo, BufferCapacity) ||
        message->Header.PayloadLength < sizeof(YUMEDISK_QUERY_INFO)) {
        return FALSE;
    }

    signatureMatch = RtlCompareMemory(
        info->AdapterSignature,
        YUMEDISK_MINIPORT_SIGNATURE,
        sizeof(info->AdapterSignature));
    serviceNameMatch = RtlCompareMemory(
        info->ServiceName,
        ControlMiniportServiceName,
        sizeof(ControlMiniportServiceName));

    if (info->ProtocolVersion != YUMEDISK_PROTOCOL_VERSION ||
        (signatureMatch != sizeof(info->AdapterSignature) &&
         serviceNameMatch != sizeof(ControlMiniportServiceName))) {
        return FALSE;
    }

    *EffectiveInformation = sizeof(SRB_IO_CONTROL) + message->Header.Size;
    return TRUE;
}

static
VOID
ControlCloseTransportSlot(
    _Inout_ PCTRL_TRANSPORT_SLOT Slot
)
{
    if (Slot->Handle != NULL) {
        ZwClose(Slot->Handle);
        Slot->Handle = NULL;
    }

    if (Slot->IoctlBuffer != NULL) {
        ControlFree(Slot->IoctlBuffer);
        Slot->IoctlBuffer = NULL;
        Slot->IoctlBufferCapacity = 0;
    }
}

static
NTSTATUS
ControlEnsureIoctlBufferCapacity(
    _Inout_ PCTRL_TRANSPORT_SLOT Slot,
    _In_ ULONG BufferCapacity
)
{
    SIZE_T allocationSize;
    PUCHAR buffer;

    if (Slot->IoctlBufferCapacity >= BufferCapacity) {
        return STATUS_SUCCESS;
    }

    allocationSize = sizeof(SRB_IO_CONTROL) + (SIZE_T)BufferCapacity;
    if (allocationSize < BufferCapacity) {
        return STATUS_INTEGER_OVERFLOW;
    }

    buffer = (PUCHAR)ControlAlloc(allocationSize);
    if (buffer == NULL) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    if (Slot->IoctlBuffer != NULL) {
        ControlFree(Slot->IoctlBuffer);
    }

    Slot->IoctlBuffer = buffer;
    Slot->IoctlBufferCapacity = BufferCapacity;
    return STATUS_SUCCESS;
}

static
NTSTATUS
ControlSendMiniportBuffer(
    _Inout_ PCTRL_TRANSPORT_SLOT Slot,
    _Inout_updates_bytes_(BufferCapacity) PUCHAR Buffer,
    _In_ ULONG InputLength,
    _In_ ULONG BufferCapacity,
    _Out_ ULONG* BytesReturned
)
{
    SIZE_T ioctlBufferSize;
    PUCHAR ioctlBuffer;
    PSRB_IO_CONTROL srbIoControl;
    IO_STATUS_BLOCK ioStatus;
    NTSTATUS status;
    ULONG transferLength;
    PYUMEDISK_MESSAGE message;
    ULONG command;

    *BytesReturned = 0;
    command = ((PYUMEDISK_MESSAGE)Buffer)->Header.Command;

    if (Slot->Handle == NULL ||
        Buffer == NULL ||
        BufferCapacity < (ULONG)YUMEDISK_MESSAGE_BASE_SIZE ||
        InputLength < (ULONG)YUMEDISK_MESSAGE_BASE_SIZE ||
        InputLength > BufferCapacity) {
        YD_KMDF_ERR(
            "ControlSendMiniportBuffer invalid parameter, cmd=%lu handle=%p input=%lu capacity=%lu",
            command,
            Slot->Handle,
            InputLength,
            BufferCapacity);
        return STATUS_INVALID_PARAMETER;
    }

    status = ControlEnsureIoctlBufferCapacity(Slot, BufferCapacity);
    if (!NT_SUCCESS(status)) {
        YD_KMDF_ERR(
            "ControlEnsureIoctlBufferCapacity failed, cmd=%lu capacity=%lu status=0x%08X",
            command,
            BufferCapacity,
            status);
        return status;
    }

    ioctlBufferSize = sizeof(SRB_IO_CONTROL) + (SIZE_T)BufferCapacity;
    ioctlBuffer = Slot->IoctlBuffer;
    RtlZeroMemory(ioctlBuffer, ioctlBufferSize);

    srbIoControl = (PSRB_IO_CONTROL)ioctlBuffer;
    srbIoControl->HeaderLength = sizeof(SRB_IO_CONTROL);
    srbIoControl->Timeout = YUMEDISK_MINIPORT_TIMEOUT_SEC;
    srbIoControl->ControlCode = YUMEDISK_MINIPORT_CONTROL_CODE;
    srbIoControl->Length = BufferCapacity;
    RtlCopyMemory(srbIoControl->Signature, YUMEDISK_MINIPORT_SIGNATURE, sizeof(srbIoControl->Signature));
    RtlCopyMemory(srbIoControl + 1, Buffer, InputLength);
    ioStatus.Status = STATUS_UNSUCCESSFUL;
    ioStatus.Information = 0;

    status = ZwDeviceIoControlFile(
        Slot->Handle,
        NULL,
        NULL,
        NULL,
        &ioStatus,
        IOCTL_SCSI_MINIPORT,
        ioctlBuffer,
        (ULONG)ioctlBufferSize,
        ioctlBuffer,
        (ULONG)ioctlBufferSize
    );

    if (NT_SUCCESS(status)) {
        status = ioStatus.Status;
    }
    if (!NT_SUCCESS(status)) {
        ULONG recoveredInformation;
        PYUMEDISK_MESSAGE ioctlMessage;

        ioctlMessage = (PYUMEDISK_MESSAGE)(srbIoControl + 1);
        YD_KMDF_LOG(
            "Miniport failed reply snapshot, cmd=%lu returnCode=0x%08X ioctlLength=%lu info=%Iu msgSize=%lu msgVersion=%lu msgCommand=%lu msgStatus=0x%08X msgSession=%I64u msgPayload=%lu",
            command,
            srbIoControl->ReturnCode,
            srbIoControl->Length,
            ioStatus.Information,
            ioctlMessage->Header.Size,
            ioctlMessage->Header.Version,
            ioctlMessage->Header.Command,
            ioctlMessage->Header.Status,
            ioctlMessage->Header.SessionId,
            ioctlMessage->Header.PayloadLength);

        recoveredInformation = 0;
        if (command == YumeDiskCommandQueryInfo &&
            ControlTryRecoverFailedQueryInfo(srbIoControl, BufferCapacity, &recoveredInformation)) {
            YD_KMDF_LOG(
                "Recovering QueryInfo reply despite failing ioctl status, handle=%p status=0x%08X recoveredInfo=%lu",
                Slot->Handle,
                status,
                recoveredInformation);
            ioStatus.Information = recoveredInformation;
            status = STATUS_SUCCESS;
        }
    }

    if (!NT_SUCCESS(status)) {
        YD_KMDF_ERR(
            "ZwDeviceIoControlFile failed, cmd=%lu handle=%p status=0x%08X ioStatus=0x%08X info=%Iu",
            command,
            Slot->Handle,
            status,
            ioStatus.Status,
            ioStatus.Information);
    }

    if (NT_SUCCESS(status)) {
        if (ioStatus.Information < sizeof(SRB_IO_CONTROL) + YUMEDISK_MESSAGE_BASE_SIZE) {
            YD_KMDF_ERR(
                "Miniport reply too short, cmd=%lu info=%Iu",
                command,
                ioStatus.Information);
            status = STATUS_DEVICE_PROTOCOL_ERROR;
        } else {
            PYUMEDISK_MESSAGE ioctlMessage;

            ioctlMessage = (PYUMEDISK_MESSAGE)(srbIoControl + 1);
            YD_KMDF_LOG(
                "Miniport raw reply, cmd=%lu returnCode=0x%08X ioctlLength=%lu info=%Iu msgSize=%lu msgVersion=%lu msgCommand=%lu msgStatus=0x%08X msgSession=%I64u msgPayload=%lu",
                command,
                srbIoControl->ReturnCode,
                srbIoControl->Length,
                ioStatus.Information,
                ioctlMessage->Header.Size,
                ioctlMessage->Header.Version,
                ioctlMessage->Header.Command,
                ioctlMessage->Header.Status,
                ioctlMessage->Header.SessionId,
                ioctlMessage->Header.PayloadLength);

            transferLength = (ULONG)min(
                (ULONG_PTR)(ioStatus.Information - sizeof(SRB_IO_CONTROL)),
                (ULONG_PTR)BufferCapacity);
            RtlCopyMemory(Buffer, srbIoControl + 1, transferLength);
            *BytesReturned = transferLength;

            message = (PYUMEDISK_MESSAGE)Buffer;
            YD_KMDF_LOG(
                "Copied reply, cmd=%lu transfer=%lu msgSize=%lu msgVersion=%lu msgCommand=%lu msgStatus=0x%08X msgSession=%I64u msgPayload=%lu",
                command,
                transferLength,
                message->Header.Size,
                message->Header.Version,
                message->Header.Command,
                message->Header.Status,
                message->Header.SessionId,
                message->Header.PayloadLength);
            if (transferLength < (ULONG)YUMEDISK_MESSAGE_BASE_SIZE ||
                message->Header.Size > transferLength) {
                YD_KMDF_ERR(
                    "Miniport protocol error, cmd=%lu transfer=%lu headerSize=%lu",
                    command,
                    transferLength,
                    (transferLength >= (ULONG)YUMEDISK_MESSAGE_BASE_SIZE) ? message->Header.Size : 0);
                status = STATUS_DEVICE_PROTOCOL_ERROR;
            }
        }
    }

    return status;
}

static
NTSTATUS
ControlProbeMiniportHandle(
    _Inout_ PCTRL_TRANSPORT_SLOT Slot
)
{
    UCHAR buffer[YUMEDISK_MESSAGE_BASE_SIZE + sizeof(YUMEDISK_QUERY_INFO)];
    PYUMEDISK_MESSAGE message;
    PYUMEDISK_QUERY_INFO info;
    ULONG bytesReturned;
    NTSTATUS status;
    SIZE_T signatureMatch;
    SIZE_T serviceNameMatch;

    RtlZeroMemory(buffer, sizeof(buffer));
    message = (PYUMEDISK_MESSAGE)buffer;
    message->Header.Size = sizeof(buffer);
    message->Header.Version = YUMEDISK_PROTOCOL_VERSION;
    message->Header.Command = YumeDiskCommandQueryInfo;

    status = ControlSendMiniportBuffer(
        Slot,
        buffer,
        YUMEDISK_MESSAGE_BASE_SIZE,
        sizeof(buffer),
        &bytesReturned);
    if (!NT_SUCCESS(status)) {
        YD_KMDF_ERR("ControlProbeMiniportHandle send failed, handle=%p status=0x%08X", Slot->Handle, status);
        return status;
    }

    if (bytesReturned < YUMEDISK_MESSAGE_BASE_SIZE + sizeof(YUMEDISK_QUERY_INFO)) {
        YD_KMDF_ERR("ControlProbeMiniportHandle short reply, handle=%p bytes=%lu", Slot->Handle, bytesReturned);
        return STATUS_DEVICE_PROTOCOL_ERROR;
    }

    info = (PYUMEDISK_QUERY_INFO)message->Payload;
    signatureMatch = RtlCompareMemory(
        info->AdapterSignature,
        YUMEDISK_MINIPORT_SIGNATURE,
        sizeof(info->AdapterSignature));
    serviceNameMatch = RtlCompareMemory(
        info->ServiceName,
        ControlMiniportServiceName,
        sizeof(ControlMiniportServiceName));

    YD_KMDF_LOG(
        "QueryInfo reply, handle=%p protocol=%lu maxTargets=%lu features=0x%08X signature=%02X %02X %02X %02X %02X %02X %02X %02X service=%ws",
        Slot->Handle,
        info->ProtocolVersion,
        info->MaxTargets,
        info->Features,
        (UCHAR)info->AdapterSignature[0],
        (UCHAR)info->AdapterSignature[1],
        (UCHAR)info->AdapterSignature[2],
        (UCHAR)info->AdapterSignature[3],
        (UCHAR)info->AdapterSignature[4],
        (UCHAR)info->AdapterSignature[5],
        (UCHAR)info->AdapterSignature[6],
        (UCHAR)info->AdapterSignature[7],
        info->ServiceName);

    if (info->ProtocolVersion != YUMEDISK_PROTOCOL_VERSION ||
        (signatureMatch != sizeof(info->AdapterSignature) &&
         serviceNameMatch != sizeof(ControlMiniportServiceName))) {
        YD_KMDF_ERR(
            "ControlProbeMiniportHandle identity mismatch, handle=%p protocol=%lu signatureMatch=%Iu serviceMatch=%Iu",
            Slot->Handle,
            info->ProtocolVersion,
            signatureMatch,
            serviceNameMatch);
        return STATUS_NOT_FOUND;
    }

    YD_KMDF_LOG("ControlProbeMiniportHandle ok, handle=%p", Slot->Handle);
    return STATUS_SUCCESS;
}

static
NTSTATUS
ControlOpenMiniportHandleByName(
    _In_ PUNICODE_STRING DeviceName,
    _Out_ HANDLE* Handle
)
{
    OBJECT_ATTRIBUTES attributes;
    IO_STATUS_BLOCK ioStatus;

    *Handle = NULL;

    InitializeObjectAttributes(
        &attributes,
        DeviceName,
        OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE,
        NULL,
        NULL);

    return ZwCreateFile(
        Handle,
        GENERIC_READ | GENERIC_WRITE | SYNCHRONIZE,
        &attributes,
        &ioStatus,
        NULL,
        FILE_ATTRIBUTE_NORMAL,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        FILE_OPEN,
        FILE_NON_DIRECTORY_FILE | FILE_SYNCHRONOUS_IO_NONALERT,
        NULL,
        0);
}

static
NTSTATUS
ControlOpenMiniportSessionSlots(
    _Inout_ PCTRL_DEVICE_CONTEXT Context
)
{
    PWSTR interfaces;
    PWSTR current;
    NTSTATUS status;
    ULONG slotIndex;
    ULONG openSlotCount;

    interfaces = NULL;
    status = IoGetDeviceInterfaces((LPGUID)&ControlStoragePortGuid, NULL, 0, &interfaces);
    if (!NT_SUCCESS(status)) {
        YD_KMDF_ERR("IoGetDeviceInterfaces(storageport) failed, status=0x%08X", status);
        return status;
    }

    YD_KMDF_LOG("ControlOpenMiniportSessionSlots begin%s", "");
    status = STATUS_NO_SUCH_DEVICE;
    current = interfaces;
    while (*current != UNICODE_NULL) {
        UNICODE_STRING deviceName;
        CTRL_TRANSPORT_SLOT probeSlot;

        RtlZeroMemory(&probeSlot, sizeof(probeSlot));
        ExInitializeFastMutex(&probeSlot.Lock);

        RtlInitUnicodeString(&deviceName, current);
        YD_KMDF_LOG("Probing storage interface %wZ", &deviceName);
        status = ControlOpenMiniportHandleByName(&deviceName, &probeSlot.Handle);
        if (!NT_SUCCESS(status)) {
            YD_KMDF_ERR("Open storage interface failed, name=%wZ status=0x%08X", &deviceName, status);
            current += wcslen(current) + 1;
            continue;
        }

        status = ControlProbeMiniportHandle(&probeSlot);
        if (!NT_SUCCESS(status)) {
            YD_KMDF_ERR("Probe storage interface failed, name=%wZ status=0x%08X", &deviceName, status);
            ControlCloseTransportSlot(&probeSlot);
            current += wcslen(current) + 1;
            continue;
        }

        Context->TransportSlots[0].Handle = probeSlot.Handle;
        probeSlot.Handle = NULL;

        openSlotCount = 1;
        for (slotIndex = 1; slotIndex < YUMEDISK_TRANSPORT_SLOT_COUNT; ++slotIndex) {
            status = ControlOpenMiniportHandleByName(&deviceName, &Context->TransportSlots[slotIndex].Handle);
            if (!NT_SUCCESS(status)) {
                Context->TransportSlots[slotIndex].Handle = NULL;
                YD_KMDF_ERR(
                    "Additional storage handle open failed, slot=%lu name=%wZ status=0x%08X",
                    slotIndex,
                    &deviceName,
                    status);
                continue;
            }

            ++openSlotCount;
        }

        ControlCloseTransportSlot(&probeSlot);

        Context->TransportOnline = TRUE;
        Context->TransportNextSlot = 0;
        Context->TransportOpenSlotCount = openSlotCount;
        YD_KMDF_LOG("Transport session online, openSlots=%lu, interface=%wZ", openSlotCount, &deviceName);
        status = STATUS_SUCCESS;
        break;
    }

    if (!NT_SUCCESS(status)) {
        YD_KMDF_ERR("ControlOpenMiniportSessionSlots failed, status=0x%08X", status);
        for (slotIndex = 0; slotIndex < YUMEDISK_TRANSPORT_SLOT_COUNT; ++slotIndex) {
            ControlCloseTransportSlot(&Context->TransportSlots[slotIndex]);
        }
    }

    ExFreePool(interfaces);
    return status;
}

static
PCTRL_TRANSPORT_SLOT
ControlAcquireTransportSlot(
    _Inout_ PCTRL_DEVICE_CONTEXT Context
)
{
    ULONG baseIndex;
    ULONG attempt;
    PCTRL_TRANSPORT_SLOT slot;

    if (Context->TransportOpenSlotCount == 0) {
        return NULL;
    }

    baseIndex = (ULONG)InterlockedIncrement(&Context->TransportNextSlot);
    for (attempt = 0; attempt < YUMEDISK_TRANSPORT_SLOT_COUNT; ++attempt) {
        slot = &Context->TransportSlots[(baseIndex + attempt) % YUMEDISK_TRANSPORT_SLOT_COUNT];
        if (slot->Handle == NULL) {
            continue;
        }
        if (ExTryToAcquireFastMutex(&slot->Lock)) {
            return slot;
        }
    }

    for (attempt = 0; attempt < YUMEDISK_TRANSPORT_SLOT_COUNT; ++attempt) {
        slot = &Context->TransportSlots[(baseIndex + attempt) % YUMEDISK_TRANSPORT_SLOT_COUNT];
        if (slot->Handle == NULL) {
            continue;
        }

        ExAcquireFastMutex(&slot->Lock);
        if (slot->Handle != NULL) {
            return slot;
        }
        ExReleaseFastMutex(&slot->Lock);
    }

    return NULL;
}

VOID
ControlTransportInitialize(
    _Out_ PCTRL_DEVICE_CONTEXT Context
)
{
    ULONG slotIndex;

    ExInitializeFastMutex(&Context->TransportStateLock);
    Context->TransportNextSlot = 0;
    Context->TransportOpenSlotCount = 0;
    Context->TransportOnline = FALSE;

    for (slotIndex = 0; slotIndex < YUMEDISK_TRANSPORT_SLOT_COUNT; ++slotIndex) {
        ExInitializeFastMutex(&Context->TransportSlots[slotIndex].Lock);
        Context->TransportSlots[slotIndex].Handle = NULL;
        Context->TransportSlots[slotIndex].IoctlBuffer = NULL;
        Context->TransportSlots[slotIndex].IoctlBufferCapacity = 0;
    }
}

NTSTATUS
ControlTransportOpenSession(
    _Inout_ PCTRL_DEVICE_CONTEXT Context
)
{
    NTSTATUS status;

    ExAcquireFastMutex(&Context->TransportStateLock);
    if (Context->TransportOnline) {
        YD_KMDF_LOG("ControlTransportOpenSession reuse existing session, openSlots=%lu", Context->TransportOpenSlotCount);
        ExReleaseFastMutex(&Context->TransportStateLock);
        return STATUS_SUCCESS;
    }

    YD_KMDF_LOG("ControlTransportOpenSession opening%s", "");
    status = ControlOpenMiniportSessionSlots(Context);
    YD_KMDF_LOG(
        "ControlTransportOpenSession result, status=0x%08X, online=%lu, openSlots=%lu",
        status,
        Context->TransportOnline,
        Context->TransportOpenSlotCount);
    ExReleaseFastMutex(&Context->TransportStateLock);
    return status;
}

VOID
ControlTransportCloseSession(
    _Inout_ PCTRL_DEVICE_CONTEXT Context
)
{
    ULONG slotIndex;

    ExAcquireFastMutex(&Context->TransportStateLock);
    YD_KMDF_LOG("ControlTransportCloseSession, openSlots=%lu", Context->TransportOpenSlotCount);
    Context->TransportOnline = FALSE;
    Context->TransportNextSlot = 0;
    Context->TransportOpenSlotCount = 0;

    for (slotIndex = 0; slotIndex < YUMEDISK_TRANSPORT_SLOT_COUNT; ++slotIndex) {
        ExAcquireFastMutex(&Context->TransportSlots[slotIndex].Lock);
        ControlCloseTransportSlot(&Context->TransportSlots[slotIndex]);
        ExReleaseFastMutex(&Context->TransportSlots[slotIndex].Lock);
    }

    ExReleaseFastMutex(&Context->TransportStateLock);
}

BOOLEAN
ControlTransportIsOnline(
    _In_ PCTRL_DEVICE_CONTEXT Context
)
{
    return Context->TransportOnline ? TRUE : FALSE;
}

NTSTATUS
ControlProxyCommand(
    _In_ PCTRL_DEVICE_CONTEXT Context,
    _Inout_updates_bytes_(BufferCapacity) PUCHAR Buffer,
    _In_ ULONG InputLength,
    _In_ ULONG BufferCapacity,
    _Out_ ULONG* BytesReturned
)
{
    PCTRL_TRANSPORT_SLOT slot;
    NTSTATUS status;

    if (BytesReturned == NULL) {
        return STATUS_INVALID_PARAMETER;
    }

    *BytesReturned = 0;

    if (!Context->TransportOnline) {
        YD_KMDF_ERR("ControlProxyCommand no transport session, input=%lu capacity=%lu", InputLength, BufferCapacity);
        return STATUS_DEVICE_NOT_READY;
    }

    slot = ControlAcquireTransportSlot(Context);
    if (slot == NULL) {
        YD_KMDF_ERR("ControlAcquireTransportSlot returned NULL%s", "");
        return STATUS_DEVICE_NOT_READY;
    }

    status = ControlSendMiniportBuffer(slot, Buffer, InputLength, BufferCapacity, BytesReturned);
    if (!NT_SUCCESS(status)) {
        YD_KMDF_ERR(
            "ControlProxyCommand send failed, cmd=%lu slotHandle=%p status=0x%08X",
            ((PYUMEDISK_MESSAGE)Buffer)->Header.Command,
            slot->Handle,
            status);
    }
    ExReleaseFastMutex(&slot->Lock);
    return status;
}

VOID
ControlSendSessionCleanup(
    _In_ PCTRL_DEVICE_CONTEXT Context,
    _In_ UINT64 SessionId
)
{
    UCHAR buffer[YUMEDISK_MESSAGE_BASE_SIZE];
    PYUMEDISK_MESSAGE message;
    ULONG bytesReturned;

    RtlZeroMemory(buffer, sizeof(buffer));
    message = (PYUMEDISK_MESSAGE)buffer;
    message->Header.Size = sizeof(buffer);
    message->Header.Version = YUMEDISK_PROTOCOL_VERSION;
    message->Header.Command = YumeDiskCommandRemoveAllDisks;
    message->Header.SessionId = SessionId;
    message->Header.Flags = YUMEDISK_SESSION_CLOSE_FLAG;

    YD_KMDF_LOG("ControlSendSessionCleanup, sessionId=%I64u", SessionId);
    (VOID)ControlProxyCommand(Context, buffer, sizeof(buffer), sizeof(buffer), &bytesReturned);
}
