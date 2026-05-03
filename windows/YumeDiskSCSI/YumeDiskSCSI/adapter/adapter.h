#pragma once

#include "..\control\control.h"
#include "..\scsi\scsi.h"

NTSTATUS
DiskDriverEntry(
    _In_ PDRIVER_OBJECT DriverObject,
    _In_ PUNICODE_STRING RegistryPath
);

