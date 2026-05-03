#ifndef _DISK_DEV_H
#define _DISK_DEV_H

#include <ntdddisk.h>

#include "define.h"

NTSTATUS DiskAdd(WDFDRIVER Driver, PWDFDEVICE_INIT DeviceInit);

NTSTATUS IoctlDiskGetDriveGeometry(PDEVICE_CONTEXT context, WDFREQUEST Request, size_t OutputBufferLength, size_t* retSize);
NTSTATUS IoctlDiskGetLengthInfo(PDEVICE_CONTEXT context, WDFREQUEST Request, size_t OutputBufferLength, size_t* retSize);
NTSTATUS IoctlStorageGetHotplugInfo(WDFREQUEST Request, size_t OutputBufferLength, size_t* retSize);
NTSTATUS IoctlStorageGetDeviceNumber(PDEVICE_CONTEXT context, WDFREQUEST Request, size_t OutputBufferLength, size_t* retSize);
NTSTATUS IoctlDiskGetPartitionInfo(PDEVICE_CONTEXT context, WDFREQUEST Request, size_t OutputBufferLength, size_t* retSize);
NTSTATUS IoctlDiskGetPartitionInfoEx(PDEVICE_CONTEXT context, WDFREQUEST Request, size_t OutputBufferLength, size_t* retSize);
NTSTATUS IoctlVolumeGetGPTAttributes(PDEVICE_CONTEXT context, WDFREQUEST Request, size_t OutputBufferLength, size_t* retSize);
NTSTATUS IoctlVolumeGetVolumeDiskExtents(PDEVICE_CONTEXT context, WDFREQUEST Request, size_t OutputBufferLength, size_t* retSize);
NTSTATUS IoctlMountdevQueryDeviceName(PDEVICE_CONTEXT context, WDFREQUEST Request, size_t OutputBufferLength, size_t* retSize);

#endif // !_DISK_DEV_H
