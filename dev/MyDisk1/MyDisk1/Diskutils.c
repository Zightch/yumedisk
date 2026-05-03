#include <ntddk.h>
#include <ntdddisk.h>
#include <stdio.h>

#include "Disk.h"
#include "Utils.h"

#pragma warning(disable: 4996)

void PassLevelGetDeviceInterfaces(void* context) {
    DbgBreakPoint();
    struct __tmp_context {
        WORK_QUEUE_ITEM workItem;
        WDFREQUEST Request;
        size_t OutputBufferLength;
    }*c = context;

    NTSTATUS stat = STATUS_SUCCESS;

    PZZWSTR list = NULL;
    stat = IoGetDeviceInterfaces(&GUID_DEVINTERFACE_DISK, NULL, DEVICE_INTERFACE_INCLUDE_NONACTIVE, &list);
    if (!NT_SUCCESS(stat)) {
        if (list != NULL) {
            ExFreePool(list);
            list = NULL;
        }
        WdfRequestComplete(c->Request, stat);
        free(c);
        return;
    }

    ULONG devNum = 0;
    size_t index = 0;
    while (TRUE) {
        size_t len = wcslen(list + index);
        if (len == 0)
            break;

        devNum++;
        index += len + 1;
    }
    ExFreePool(list);

    UINT64 numberSize = sizeof(STORAGE_DEVICE_NUMBER);
    if (c->OutputBufferLength < numberSize) {
        WdfRequestComplete(c->Request, STATUS_BUFFER_TOO_SMALL);
        free(c);
        return;
    }

    PSTORAGE_DEVICE_NUMBER number;
    size_t length;
    stat = WdfRequestRetrieveOutputBuffer(c->Request, c->OutputBufferLength, (PVOID*)&number, &length);
    if (!NT_SUCCESS(stat)) {
        WdfRequestComplete(c->Request, stat);
        free(c);
        return;
    }

    DbgBreakPoint();
    number->DeviceType = FILE_DEVICE_DISK;
    number->DeviceNumber = devNum;
    number->PartitionNumber = (ULONG)(-1);

    WdfRequestCompleteWithInformation(c->Request, stat, numberSize);
    free(c);
}

void GetDiskNum(WDFREQUEST Request, size_t OutputBufferLength) {
    DbgBreakPoint();
    struct __tmp_context {
        WORK_QUEUE_ITEM workItem;
        WDFREQUEST Request;
        size_t OutputBufferLength;
    } *context = malloc(sizeof(struct __tmp_context));

    if (context == NULL) {
        WdfRequestComplete(Request, STATUS_INSUFFICIENT_RESOURCES);
        return;
    }

    context->OutputBufferLength = OutputBufferLength;
    context->Request = Request;

    ExInitializeWorkItem(&context->workItem, PassLevelGetDeviceInterfaces, &context);
    ExQueueWorkItem(&context->workItem, CriticalWorkQueue); // Á¢¼´¹¤×÷
}
