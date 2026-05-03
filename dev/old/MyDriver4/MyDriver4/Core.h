#ifndef _CORE_H
#define _CORE_H

#include <ntddk.h>
#include "Define.h"

extern PDEVICE_OBJECT devObjCtrl;
extern MY_IRP_NODE devCtrlRequestApp; // 请求应用端口 驱动->应用
extern KSPIN_LOCK requestAppLock; // 队列自旋锁
extern LIST_ENTRY devCtrlCmdQueue; // 修复：应该是CmdQueue不是WriteQueue
extern DWORD32 cmdIDSeq; // 添加声明

void CancelCtrlRequestApp(PDEVICE_OBJECT devObj, PIRP irp);
void ClearRequestApp();

#endif
