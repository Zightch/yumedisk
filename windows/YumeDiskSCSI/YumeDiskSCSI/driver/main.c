#include <ntddk.h>

#include "..\adapter\adapter.h"
#include "..\core\defs.h"

NTSTATUS
DriverEntry(
    _In_ PDRIVER_OBJECT DriverObject,
    _In_ PUNICODE_STRING RegistryPath
)
{
    YD_SCSI_LOG("DriverEntry enter, build=%s %s", __DATE__, __TIME__);
    return DiskDriverEntry(DriverObject, RegistryPath);
}

