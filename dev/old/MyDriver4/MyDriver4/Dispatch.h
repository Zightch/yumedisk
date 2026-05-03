#ifndef _DISPATCH_H
#define _DISPATCH_H

#include <ntddk.h>

NTSTATUS DispatchCreateAndClose(PDEVICE_OBJECT devObj, PIRP irp);
NTSTATUS DispatchReadAndWrite(PDEVICE_OBJECT devObj, PIRP irp);
NTSTATUS DispatchDeviceControl(PDEVICE_OBJECT devObj, PIRP irp);

#endif
