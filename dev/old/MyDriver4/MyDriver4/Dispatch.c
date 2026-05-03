#include <stdio.h>
#include "Dispatch.h"
#include "Define.h"
#include "Utils.h"
#include "Core.h"

// #pragma warning(disable: 4100)
#pragma warning(disable: 4189)

BOOLEAN devCtrlIsOpen = FALSE; // 控制端口已被打开
KSPIN_LOCK dispatchLock; // 派遣互斥对象

NTSTATUS DispatchCreateAndClose(PDEVICE_OBJECT devObj, PIRP irp) {
	KIRQL dispatchOldIrql;
	KeAcquireSpinLock(&dispatchLock, &dispatchOldIrql);

	PIO_STACK_LOCATION iosl = IoGetCurrentIrpStackLocation(irp);

	// 默认状态
	int cancelStat = CANCEL_STAT_DO_NOTHING;
	NTSTATUS dispatchStat = STATUS_SUCCESS;
	irp->IoStatus.Status = STATUS_SUCCESS; // GetLastError
	irp->IoStatus.Information = 0; // 返回信息长度

	UCHAR fnType = iosl->MajorFunction; // 获取派遣类型
	if (fnType == IRP_MJ_CREATE) {
		if (devObj == devObjCtrl) {
			if (devCtrlIsOpen == TRUE) {
				DbgPrint("%s: ctrl port open failed - already open\n", DRIVER_NAME);
				dispatchStat = STATUS_SHARING_VIOLATION;
				irp->IoStatus.Status = STATUS_SHARING_VIOLATION;
				irp->IoStatus.Information = 0;
			}
			else {
				devCtrlIsOpen = TRUE;
				devCtrlRequestApp.completedSize = 2; // 临时标记, 表示首次打开
			}
		}
		// TODO
	}
	else if (fnType == IRP_MJ_CLOSE) {
		if (devObj == devObjCtrl) {
			// 关闭requestApp
			KIRQL oldIrql;
			KeAcquireSpinLock(&requestAppLock, &oldIrql); // 上锁
			ClearRequestApp();
			KeReleaseSpinLock(&requestAppLock, oldIrql); // 解锁
			// TODO
			devCtrlIsOpen = FALSE;
		}
		// TODO
	}

	KeReleaseSpinLock(&dispatchLock, dispatchOldIrql);
	if (dispatchStat != STATUS_PENDING && cancelStat != CANCEL_STAT_CANCELLED_BY_ROUTINE)
		IoCompleteRequest(irp, IO_NO_INCREMENT);

	// TODO

	return dispatchStat;
}

NTSTATUS DispatchReadAndWrite(PDEVICE_OBJECT devObj, PIRP irp) {
	KIRQL dispatchOldIrql;
	KeAcquireSpinLock(&dispatchLock, &dispatchOldIrql);

	PIO_STACK_LOCATION iosl = IoGetCurrentIrpStackLocation(irp);

	// 默认状态
	int cancelStat = CANCEL_STAT_DO_NOTHING;
	NTSTATUS dispatchStat = STATUS_SUCCESS;
	irp->IoStatus.Status = STATUS_SUCCESS; // GetLastError
	irp->IoStatus.Information = 0; // 返回信息长度

	UCHAR fnType = iosl->MajorFunction; // 获取派遣类型
	if (fnType == IRP_MJ_READ) {
		if (devObj == devObjCtrl) {
			
		}
		// TODO
	}
	else if (fnType == IRP_MJ_WRITE) {
		if (devObj == devObjCtrl) {
			
		}
		// TODO
	}

	KeReleaseSpinLock(&dispatchLock, dispatchOldIrql);
	if (dispatchStat != STATUS_PENDING && cancelStat != CANCEL_STAT_CANCELLED_BY_ROUTINE)
		IoCompleteRequest(irp, IO_NO_INCREMENT);

	// TODO

	return dispatchStat;
}

NTSTATUS DispatchDeviceControl(PDEVICE_OBJECT devObj, PIRP irp) {
	KIRQL dispatchOldIrql;
	KeAcquireSpinLock(&dispatchLock, &dispatchOldIrql);

	PIO_STACK_LOCATION iosl = IoGetCurrentIrpStackLocation(irp);

	// 默认状态
	int cancelStat = CANCEL_STAT_DO_NOTHING;
	NTSTATUS dispatchStat = STATUS_SUCCESS;
	irp->IoStatus.Status = STATUS_SUCCESS; // GetLastError
	irp->IoStatus.Information = 0; // 返回信息长度

	UCHAR fnType = iosl->MajorFunction; // 获取派遣类型
	ULONG ctrlCode = iosl->Parameters.DeviceIoControl.IoControlCode;
	ULONG inSize = iosl->Parameters.DeviceIoControl.InputBufferLength;
	ULONG outSize = iosl->Parameters.DeviceIoControl.OutputBufferLength;
	if (devObj == devObjCtrl && ctrlCode == MSG_CODE_REQUEST_APP) { // 特判: 驱动 -> 应用 请求
		do {
			if (outSize < MIN_REQUEST_APP_BUFFER_SIZE) {
				DbgPrint("%s: backward read buffer size %lu less than minimum %lu\n", DRIVER_NAME, outSize, MIN_REQUEST_APP_BUFFER_SIZE);
				dispatchStat = STATUS_INVALID_BUFFER_SIZE;
				irp->IoStatus.Status = STATUS_INVALID_BUFFER_SIZE;
				irp->IoStatus.Information = 0;
				break;
			}
			KIRQL oldIrql;
			KeAcquireSpinLock(&requestAppLock, &oldIrql); // 上锁
			// 应用层刚打开控制端口 -> 立刻完成IRP, 写入SUFFICIENT_REQUEST_APP_IRP_NUM和WARNING_REQUEST_APP_IRP_NUM
			if (devCtrlRequestApp.completedSize == 2) {
				char* outBuffer = MmGetSystemAddressForMdlSafe(irp->MdlAddress, NormalPagePriority); // 建立映射
				int size = sprintf(outBuffer, "type: stat; data: SUFFICIENT_REQUEST_APP_IRP_NUM: %d, MIN_REQUEST_APP_IRP_NUM: %d", SUFFICIENT_REQUEST_APP_IRP_NUM, WARNING_REQUEST_APP_IRP_NUM);
				irp->IoStatus.Status = STATUS_SUCCESS;
				irp->IoStatus.Information = size;
				devCtrlRequestApp.completedSize = 1;
				KeReleaseSpinLock(&requestAppLock, oldIrql); // 解锁
				break;
			}
			if (devCtrlRequestApp.completedSize == 1 || devCtrlRequestApp.completedSize == 0) {
				// 插入队列
				PMY_IRP_NODE irpNode = ExAllocatePool2(POOL_FLAG_NON_PAGED | POOL_FLAG_UNINITIALIZED, sizeof(MY_IRP_NODE), MEM_TAG);
				if (irpNode) {
					irpNode->irp = irp;
					irpNode->fnType = fnType;
					irpNode->outBuffer = MmGetSystemAddressForMdlSafe(irp->MdlAddress, NormalPagePriority); // 建立映射
					irpNode->outSize = outSize;
					IoMarkIrpPending(irp); // 挂起
					InsertTailList(&devCtrlRequestApp.entry, &(irpNode->entry)); // 入队
					devCtrlRequestApp.outSize++; // 数量++
					if (devCtrlRequestApp.outSize >= SUFFICIENT_REQUEST_APP_IRP_NUM) {
						devCtrlRequestApp.completedSize = 0;
						DbgPrint("%s: driver ready\n", DRIVER_NAME);
					}
					// 设置取消例程
					cancelStat = SetCancelRoutine(irp, CancelCtrlRequestApp);
					if (IS_CANCELLED(cancelStat))
						dispatchStat = STATUS_CANCELLED;
					else if (cancelStat == CANCEL_STAT_SUCCESS) // 正常处理
						dispatchStat = STATUS_PENDING;
					else if (cancelStat == CANCEL_STAT_DO_NOTHING)
						DbgBreakPoint(); // 应该不会命中, 除非参数传NULL
					KeReleaseSpinLock(&requestAppLock, oldIrql); // 解锁
					// TODO 创建工作线程, 完成其他挂起IRP
				}
				else {
					KeReleaseSpinLock(&requestAppLock, oldIrql); // 解锁
					DbgPrint("%s: memory allocation failed\n", DRIVER_NAME);
					DbgBreakPoint();
					dispatchStat = STATUS_INSUFFICIENT_RESOURCES;
					irp->IoStatus.Status = STATUS_INSUFFICIENT_RESOURCES;
					irp->IoStatus.Information = 0;
				}
			}
			else {
				// 驱动状态错误
				DbgBreakPoint(); // 应该不会命中
				dispatchStat = STATUS_DEVICE_NOT_READY;
				irp->IoStatus.Status = STATUS_DEVICE_NOT_READY;
				irp->IoStatus.Information = 0;
			}
		} while (FALSE);
	}
	else {
		KIRQL oldIrql;
		KeAcquireSpinLock(&requestAppLock, &oldIrql); // 上锁
		if (devCtrlRequestApp.completedSize == 1 || (devCtrlRequestApp.completedSize == 0 && devCtrlRequestApp.outSize == 0)) {
			KeReleaseSpinLock(&requestAppLock, oldIrql); // 解锁
			// TODO 根据不同的类型挂起请求
		}
		else {
			KeReleaseSpinLock(&requestAppLock, oldIrql); // 解锁

		}
		// TODO
	}

	KeReleaseSpinLock(&dispatchLock, dispatchOldIrql);
	if (dispatchStat != STATUS_PENDING && cancelStat != CANCEL_STAT_CANCELLED_BY_ROUTINE)
		IoCompleteRequest(irp, IO_NO_INCREMENT);

	// TODO

	return dispatchStat;
}
