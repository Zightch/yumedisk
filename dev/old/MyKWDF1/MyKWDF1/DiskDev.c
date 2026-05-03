#include "DiskDev.h"
#include "define.h"

#pragma warning(disable: 4100)
#pragma warning(disable: 4189)

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(DISK_DEVICE_CONTEXT, GetDiskDeviceContext);

void DiskDevFileCreate(WDFDEVICE Device, WDFREQUEST Request, WDFFILEOBJECT FileObject) {
	NTSTATUS stat = STATUS_SUCCESS;


	WdfRequestComplete(Request, stat);
}

void DiskDevFileClose(WDFFILEOBJECT FileObject) {
	
}

void DiskDevFileCleanup(WDFFILEOBJECT FileObject) {

}

void DiskDevDeviceControl(WDFQUEUE Queue, WDFREQUEST Request, size_t OutputBufferLength, size_t InputBufferLength, ULONG IoControlCode) {
	NTSTATUS stat = STATUS_SUCCESS;

	// TODO

	WdfRequestComplete(Request, STATUS_SUCCESS);
}
