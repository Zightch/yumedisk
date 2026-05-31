#include "event.h"

#include "..\\slot\\slot.h"

VOID
DiskCompletePendingEventSlot(
    _In_ PVOID DeviceExtension,
    _In_ ULONG TargetId,
    _In_ NTSTATUS Status,
    _In_opt_ const YUMEDISK_DISK_EVENT* EventRecord
)
{
    PDEVICE_CONTEXT extension;
    PYUME_DISK disk;
    PSTORAGE_REQUEST_BLOCK pendingSrb;
    UINT64 kernelVa;
    ULONG capacity;
    KIRQL oldIrql;

    extension = (PDEVICE_CONTEXT)DeviceExtension;
    if (TargetId >= extension->MaxTargets) {
        return;
    }

    disk = &extension->Disk[TargetId];
    pendingSrb = NULL;
    kernelVa = 0ull;
    capacity = 0u;

    KeAcquireSpinLock(&disk->EventSlot.Lock, &oldIrql);
    pendingSrb = disk->EventSlot.PendingSrb;
    if (pendingSrb != NULL) {
        kernelVa = disk->EventSlot.KernelVa;
        capacity = disk->EventSlot.Capacity;
        disk->EventSlot.PendingSrb = NULL;
        disk->EventSlot.SlotId = 0ull;
        disk->EventSlot.KernelVa = 0ull;
        disk->EventSlot.Capacity = 0u;
        disk->EventSlot.Flags = 0u;
    }
    KeReleaseSpinLock(&disk->EventSlot.Lock, oldIrql);

    if (pendingSrb == NULL) {
        return;
    }

    DiskCompleteEventSlotSrb(
        DeviceExtension,
        (UCHAR)TargetId,
        pendingSrb,
        kernelVa,
        capacity,
        Status,
        EventRecord);
}

NTSTATUS
DiskHandleSubmitEventSlotIoctl(
    _In_ PVOID DeviceExtension,
    _In_ PSTORAGE_REQUEST_BLOCK Srb,
    _Inout_ PYUMEDISK_MESSAGE Message
)
{
    PDEVICE_CONTEXT extension;
    PYUMEDISK_SUBMIT_EVENT_SLOT submitEventSlot;
    YUMEDISK_EVENT_SLOT_DESCRIPTOR* slotDescriptor;
    PYUME_DISK disk;
    KIRQL oldIrql;
    NTSTATUS status;

    extension = (PDEVICE_CONTEXT)DeviceExtension;
    if (Message->Header.PayloadLength < sizeof(*submitEventSlot)) {
        return STATUS_INVALID_PARAMETER;
    }

    submitEventSlot = (PYUMEDISK_SUBMIT_EVENT_SLOT)Message->Payload;
    slotDescriptor = &submitEventSlot->Slot;
    if (slotDescriptor->SessionId != Message->Header.SessionId ||
        slotDescriptor->TargetId != Message->Header.TargetId ||
        slotDescriptor->TargetId >= extension->MaxTargets ||
        !DiskIsUsableTargetId(slotDescriptor->TargetId) ||
        slotDescriptor->SlotId == 0ull ||
        slotDescriptor->KernelVa == 0ull ||
        slotDescriptor->Capacity < sizeof(YUMEDISK_DISK_EVENT)) {
        return STATUS_INVALID_PARAMETER;
    }

    disk = &extension->Disk[slotDescriptor->TargetId];
    if (!disk->Present || !disk->Configured) {
        return STATUS_DEVICE_NOT_CONNECTED;
    }

    status = STATUS_PENDING;
    KeAcquireSpinLock(&disk->EventSlot.Lock, &oldIrql);
    if (disk->EventSlot.PendingSrb != NULL) {
        status = STATUS_DEVICE_BUSY;
    } else {
        disk->EventSlot.PendingSrb = Srb;
        disk->EventSlot.SlotId = slotDescriptor->SlotId;
        disk->EventSlot.KernelVa = slotDescriptor->KernelVa;
        disk->EventSlot.Capacity = slotDescriptor->Capacity;
        disk->EventSlot.Flags = slotDescriptor->Flags;
    }
    KeReleaseSpinLock(&disk->EventSlot.Lock, oldIrql);

    return status;
}
