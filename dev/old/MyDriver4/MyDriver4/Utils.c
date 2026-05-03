#include "Utils.h"
#include "Define.h"

// #pragma warning(disable: 4100)
// #pragma warning(disable: 4189)

int SetCancelRoutine(PIRP irp, PDRIVER_CANCEL cancelRoutine) {
	if (cancelRoutine == NULL)
		return CANCEL_STAT_DO_NOTHING;

	int stat = CANCEL_STAT_SUCCESS;
	IoSetCancelRoutine(irp, cancelRoutine);
	if (irp->Cancel) { // 如果取消了
		if (IoSetCancelRoutine(irp, NULL) != NULL) { // 情况1: 取消例程没有被调用 -> 手动释放
			stat = CANCEL_STAT_CANCELLED_BY_SELF;
			irp->IoStatus.Status = STATUS_CANCELLED;
			irp->IoStatus.Information = 0;
		}
		else
			// 情况2: 取消例程已在进行中 -> 什么也不用做
			stat = CANCEL_STAT_CANCELLED_BY_ROUTINE;
	}
	return stat;
}
