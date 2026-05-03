#ifndef _UTILS_H
#define _UTILS_H

#include <ntddk.h>

/*
 * 该函数必须在cancelRoutine持有的锁中执行, 以确保线程安全
 * 例如:
 *   KeAcquireSpinLock(&lock, &oldIrql); // 上锁
 *   ...
 *   cancelStat = SetCancelRoutine(irp, cancelRoutine);
 *   ...
 *   KeReleaseSpinLock(&lock, oldIrql); // 解锁
 * 此lock必须与cancelRoutine持有的lock相同
 */
int SetCancelRoutine(PIRP irp, PDRIVER_CANCEL cancelRoutine);

#endif
