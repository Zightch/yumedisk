#ifndef _DISK_DEV_H
#define _DISK_DEV_H

#include <ntddk.h>
#include <wdf.h>

void DiskDevFileCreate(WDFDEVICE Device, WDFREQUEST Request, WDFFILEOBJECT FileObject);
void DiskDevFileClose(WDFFILEOBJECT FileObject);
void DiskDevFileCleanup(WDFFILEOBJECT FileObject);

void DiskDevDeviceControl(WDFQUEUE Queue, WDFREQUEST Request, size_t OutputBufferLength, size_t InputBufferLength, ULONG IoControlCode);

#endif // !_DISK_DEV_H
