#include <ntddk.h>

#include "..\adapter\adapter.h"

NTSTATUS
DriverEntry(
    _In_ PDRIVER_OBJECT DriverObject,
    _In_ PUNICODE_STRING RegistryPath
)
{
    return DiskDriverEntry(DriverObject, RegistryPath);
}

