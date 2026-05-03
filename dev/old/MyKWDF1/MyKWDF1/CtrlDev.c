#include "CtrlDev.h"
#include "define.h"
#include "DiskDev.h"

#pragma warning(disable: 4100)
#pragma warning(disable: 4189)

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(CTRL_DEVICE_CONTEXT, GetCtrlDeviceContext);
WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(DISK_DEVICE_CONTEXT, GetDiskDeviceContext);

NTSTATUS CreateDiskDev(WDFDEVICE ctrlDev, PWCH devName, size_t devNameLen, PWCH symlinkName, size_t symlinkNameLen) {
	if (devNameLen >= 65520 || symlinkNameLen >= 65520 || devName == NULL || symlinkName == NULL)
		return STATUS_INVALID_PARAMETER;

	WDFDRIVER driver = WdfDeviceGetDriver(ctrlDev);
	PCTRL_DEVICE_CONTEXT ctrlDevContext = GetCtrlDeviceContext(ctrlDev);

	// 初始化设备名
	UINT64 devNameLenByByte = devNameLen * sizeof(WCHAR);
	UINT64 devNameMaxLenByByte = (devNameLenByByte + 1 + INT_MUL_8_MASK) & INT_MUL_8;
	UINT64 symlinkNameLenByByte = symlinkNameLen * sizeof(WCHAR);
	UINT64 symlinkNameMaxLenByByte = (symlinkNameLenByByte + 1 + INT_MUL_8_MASK) & INT_MUL_8;

	PWCH devNamePool = ExAllocatePool2(POOL_FLAG_NON_PAGED, devNameMaxLenByByte, MEM_TAG);
	if (devNamePool == NULL)
		return STATUS_INSUFFICIENT_RESOURCES;
	memset(devNamePool, 0, devNameMaxLenByByte);

	PWCH symlinkNamePool = ExAllocatePool2(POOL_FLAG_NON_PAGED, symlinkNameMaxLenByByte, MEM_TAG);
	if (symlinkNamePool == NULL) {
		ExFreePoolWithTag(devNamePool, MEM_TAG);
		return STATUS_INSUFFICIENT_RESOURCES;
	}
	memset(symlinkNamePool, 0, symlinkNameMaxLenByByte);

	memcpy(devNamePool, devName, devNameLen * sizeof(WCHAR));
	memcpy(symlinkNamePool, symlinkName, symlinkNameLen * sizeof(WCHAR));

	UNICODE_STRING devNameUS;
	UNICODE_STRING symlinkNameUS;
	devNameUS.Buffer = devNamePool;
	devNameUS.Length = (USHORT)devNameLenByByte;
	devNameUS.MaximumLength = (USHORT)devNameMaxLenByByte;
	symlinkNameUS.Buffer = symlinkNamePool;
	symlinkNameUS.Length = (USHORT)symlinkNameLenByByte;
	symlinkNameUS.MaximumLength = (USHORT)symlinkNameMaxLenByByte;

	// 创建设备
	PWDFDEVICE_INIT devInit = WdfControlDeviceInitAllocate(driver, &SDDL_DEVOBJ_SYS_ALL_ADM_RWX_WORLD_RW_RES_R);
	if (devInit == NULL) {
		ExFreePoolWithTag(devNamePool, MEM_TAG);
		ExFreePoolWithTag(symlinkNamePool, MEM_TAG);
		return STATUS_INSUFFICIENT_RESOURCES;
	}

	NTSTATUS stat = STATUS_SUCCESS;

	stat = WdfDeviceInitAssignName(devInit, &devNameUS);
	if (!NT_SUCCESS(stat)) {
		WdfDeviceInitFree(devInit);
		ExFreePoolWithTag(devNamePool, MEM_TAG);
		ExFreePoolWithTag(symlinkNamePool, MEM_TAG);
		return stat;
	}

	WDF_FILEOBJECT_CONFIG fileConfig;
	WDF_FILEOBJECT_CONFIG_INIT(&fileConfig, DiskDevFileCreate, DiskDevFileClose, DiskDevFileCleanup);
	WdfDeviceInitSetFileObjectConfig(devInit, &fileConfig, WDF_NO_OBJECT_ATTRIBUTES);
	WdfDeviceInitSetIoType(devInit, WdfDeviceIoDirect); // 使用直接IO

	WDFDEVICE dev;
	WDF_OBJECT_ATTRIBUTES devAttributes;
	WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&devAttributes, DISK_DEVICE_CONTEXT);
	devAttributes.SynchronizationScope = WdfSynchronizationScopeDevice; // 使用设备同步
	stat = WdfDeviceCreate(&devInit, &devAttributes, &dev);
	if (!NT_SUCCESS(stat)) {
		WdfDeviceInitFree(devInit);
		ExFreePoolWithTag(devNamePool, MEM_TAG);
		ExFreePoolWithTag(symlinkNamePool, MEM_TAG);
		return stat;
	}

	// 初始化上下文
	PDISK_DEVICE_CONTEXT diskDevContext = GetDiskDeviceContext(dev);
	diskDevContext->devName = devNameUS;
	diskDevContext->symlinkName = symlinkNameUS;
	diskDevContext->id = ctrlDevContext->diskDevIDSeq;
	diskDevContext->dev = dev;

	// 设置默认队列
	WDFQUEUE defaultQueue;
	WDF_IO_QUEUE_CONFIG ioQueueConfig;
	WDF_IO_QUEUE_CONFIG_INIT_DEFAULT_QUEUE(&ioQueueConfig, WdfIoQueueDispatchSequential);
	// TODO 指派事件
	ioQueueConfig.EvtIoDeviceControl = DiskDevDeviceControl;
	stat = WdfIoQueueCreate(dev, &ioQueueConfig, WDF_NO_OBJECT_ATTRIBUTES, &defaultQueue);
	if (!NT_SUCCESS(stat)) {
		WdfObjectDelete(dev);
		ExFreePoolWithTag(devNamePool, MEM_TAG);
		ExFreePoolWithTag(symlinkNamePool, MEM_TAG);
		return stat;
	}

	DbgBreakPoint();
	stat = WdfDeviceCreateDeviceInterface(dev, &GUID_MY_DISK_DEV, NULL);
	if (!NT_SUCCESS(stat)) {
		WdfObjectDelete(dev);
		ExFreePoolWithTag(devNamePool, MEM_TAG);
		ExFreePoolWithTag(symlinkNamePool, MEM_TAG);
		DbgPrint("ctrl dev WdfDeviceCreateDeviceInterface fail: %08x\n", stat);
		return stat;
	}

	WdfControlFinishInitializing(dev);
	// 完成设备创建

	// 将新设备插入到控制设备的AVL树中
	INDEX index = { diskDevContext->id, diskDevContext };
	BOOLEAN insertOK = FALSE;
	RtlInsertElementGenericTableAvl(&ctrlDevContext->diskDevIndex, &index, sizeof(INDEX), &insertOK);
	if (insertOK == FALSE) {
		WdfObjectDelete(dev);
		ExFreePoolWithTag(devNamePool, MEM_TAG);
		ExFreePoolWithTag(symlinkNamePool, MEM_TAG);
		return STATUS_INSUFFICIENT_RESOURCES;
	}
	ctrlDevContext->diskDevIDSeq++;

	return STATUS_SUCCESS;
}

NTSTATUS RemoveDiskDev(WDFDEVICE ctrlDev, UINT64 id) {
	PCTRL_DEVICE_CONTEXT ctrlDevContext = GetCtrlDeviceContext(ctrlDev);
	INDEX find = { id, NULL };
	PINDEX result = RtlLookupElementGenericTableAvl(&ctrlDevContext->diskDevIndex, &find);
	if (result == NULL)
		return STATUS_RESOURCE_DATA_NOT_FOUND;

	PDISK_DEVICE_CONTEXT diskDevContext = (PDISK_DEVICE_CONTEXT) result->value;

	void* devNameBuffer = diskDevContext->devName.Buffer;
	void* symlinkNameBuffer = diskDevContext->symlinkName.Buffer;
	WdfObjectDelete(diskDevContext->dev);
	ExFreePoolWithTag(devNameBuffer, MEM_TAG);
	ExFreePoolWithTag(symlinkNameBuffer, MEM_TAG);

	// TODO

	RtlDeleteElementGenericTableAvl(&ctrlDevContext->diskDevIndex, result);
	return STATUS_SUCCESS;
}

void CtrlDevFileCreate(WDFDEVICE Device, WDFREQUEST Request, WDFFILEOBJECT FileObject) {
	PCTRL_DEVICE_CONTEXT context = GetCtrlDeviceContext(Device);
	NTSTATUS stat = STATUS_SUCCESS;

	WdfSpinLockAcquire(context->openLock);
	if (context->isOpen == FALSE)
		context->isOpen = TRUE;
	else
		stat = STATUS_SHARING_VIOLATION;
	WdfSpinLockRelease(context->openLock);

	WdfRequestComplete(Request, stat);
}

void CtrlDevFileClose(WDFFILEOBJECT FileObject) {
	PCTRL_DEVICE_CONTEXT context = GetCtrlDeviceContext(WdfFileObjectGetDevice(FileObject));
	WdfSpinLockAcquire(context->openLock);
	context->isOpen = FALSE;
	WdfSpinLockRelease(context->openLock);
}

void CtrlDevFileCleanup(WDFFILEOBJECT FileObject) {

}

void CtrlDevCanceled(WDFQUEUE Queue, WDFREQUEST Request) {
	WdfRequestComplete(Request, STATUS_CANCELLED);
}

void CtrlDevDeviceControl(WDFQUEUE Queue, WDFREQUEST Request, size_t OutputBufferLength, size_t InputBufferLength, ULONG IoControlCode) {
	WDFDEVICE ctrlDev = WdfIoQueueGetDevice(Queue);
	PCTRL_DEVICE_CONTEXT context = GetCtrlDeviceContext(ctrlDev);
	NTSTATUS stat = STATUS_SUCCESS;

	// TODO
	if (IoControlCode == MSG_CODE_WRITE) {
		char* createDev = "create dev";
		char* removeDev = "remove dev";
		char* buffer;
		size_t size;
		stat = WdfRequestRetrieveInputBuffer(Request, 10, &buffer, &size);
		if (NT_SUCCESS(stat)) {
			if (memcmp(buffer, createDev, 10) == 0)
				stat = CreateDiskDev(ctrlDev, L"\\Device\\MyTestDiskDev", 21, L"\\??\\Global\\MyTestDiskDev", 24);
			else if (memcmp(buffer, removeDev, 10) == 0)
				stat = RemoveDiskDev(ctrlDev, context->diskDevIDSeq - 1);
			else
				stat = STATUS_INVALID_PARAMETER;
		}
	}
	else
		stat = STATUS_INVALID_DEVICE_REQUEST;

	WdfRequestComplete(Request, stat);
}
