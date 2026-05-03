#include "Core.h"

#pragma warning(disable: 4100)
// #pragma warning(disable: 4189)

PDEVICE_OBJECT devObjCtrl = NULL;

/*
 * devCtrlRequestApp
 * 只接受DeviceControl请求
 * 必须始终存在个IRP在这个节点, 用于驱动向应用端口发送请求
 * 当应用程序打开控制端口设备后, 必须立刻向该节点投递一个读请求IRP
 * 当该节点存在时, 驱动才进入就绪态, 否则驱动程序将挂起所有其他请求
 * 
 * 同时这个也用于返回CMD任务操作结果
 */
MY_IRP_NODE devCtrlRequestApp; // 请求应用端口  驱动 -> 应用 请求APP
KSPIN_LOCK requestAppLock; // 队列自旋锁

/*
 * 该队列类型MY_CMD_NODE, 驱动在接到命令IRP后立刻完成, 派遣函数返回一个任务号, 类型DWORD(UINT32)
 * 在驱动完成命令后, 通过完成返回IRP来向应用层表示指令完成
 * 
 * 同时用于返回驱动程序请求应用层的任务
 */
LIST_ENTRY devCtrlCmdQueue; // 应用向控制端口发送命令, 这是个写队列
DWORD32 cmdIDSeq = 0; // 累计任务号ID

void ClearRequestApp() {
	while (!IsListEmpty(&devCtrlRequestApp.entry)) {
		PMY_IRP_NODE node = CONTAINING_RECORD(RemoveHeadList(&devCtrlRequestApp.entry), MY_IRP_NODE, entry);
		node->irp->IoStatus.Status = STATUS_CANCELLED;
		node->irp->IoStatus.Information = 0;
		IoCompleteRequest(node->irp, IO_NO_INCREMENT);
		devCtrlRequestApp.outSize--;
		ExFreePoolWithTag(node, MEM_TAG);
	}
}

void CancelCtrlRequestApp(PDEVICE_OBJECT devObj, PIRP irp) {
	IoReleaseCancelSpinLock(irp->CancelIrql); // 一定要释放系统自带的取消自旋锁

	KIRQL oldIrqlL;
	KeAcquireSpinLock(&requestAppLock, &oldIrqlL);
	// 遍历队列, 找到要删除的irpNode
	for (PLIST_ENTRY p = devCtrlRequestApp.entry.Flink; p != &devCtrlRequestApp.entry; p = p->Flink) {
		PMY_IRP_NODE node = CONTAINING_RECORD(p, MY_IRP_NODE, entry); // 获取节点地址
		if (node->irp == irp) {
			devCtrlRequestApp.outSize--;
			RemoveEntryList(&node->entry);
			ExFreePoolWithTag(node, MEM_TAG);
			break;
		}
	}
	KeReleaseSpinLock(&requestAppLock, oldIrqlL);

	irp->IoStatus.Status = STATUS_CANCELLED;
	irp->IoStatus.Information = 0;
	IoCompleteRequest(irp, IO_NO_INCREMENT);
}
