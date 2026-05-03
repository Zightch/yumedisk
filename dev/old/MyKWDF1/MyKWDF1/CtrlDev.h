#ifndef _CTRL_DEV_H
#define _CTRL_DEV_H

#include <ntddk.h>
#include <wdf.h>

void CtrlDevFileCreate(WDFDEVICE Device, WDFREQUEST Request, WDFFILEOBJECT FileObject);
void CtrlDevFileClose(WDFFILEOBJECT FileObject);
void CtrlDevFileCleanup(WDFFILEOBJECT FileObject);

void CtrlDevCanceled(WDFQUEUE Queue, WDFREQUEST Request);
void CtrlDevDeviceControl(WDFQUEUE Queue, WDFREQUEST Request, size_t OutputBufferLength, size_t InputBufferLength, ULONG IoControlCode);

#endif // !_CTRL_DEV_H
