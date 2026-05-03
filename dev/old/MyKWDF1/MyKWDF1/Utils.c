#include "Utils.h"
#include "define.h"

#pragma warning(disable: 4100)

RTL_GENERIC_COMPARE_RESULTS AvlCmp(PRTL_AVL_TABLE Table, PVOID FirstStruct, PVOID SecondStruct) {
	PINDEX f = (PINDEX)FirstStruct;
	PINDEX s = (PINDEX)SecondStruct;
	if (f->key < s->key)return GenericLessThan;
	if (f->key > s->key)return GenericGreaterThan;
	return GenericEqual;
}

PVOID AvlAlloc(PRTL_AVL_TABLE Table, CLONG  ByteSize) {
	return ExAllocatePool2(POOL_FLAG_NON_PAGED, ByteSize, MEM_TAG);
}

void AvlFree(PRTL_AVL_TABLE Table, PVOID  Buffer) {
	ExFreePoolWithTag(Buffer, MEM_TAG);
}

void FastMemcpy(char* d, char* s, size_t size) {
	size_t size8 = size >> 3; // 以8字节拷贝多少次
	size_t front = size & INT_MUL_8; // 以8字节拷贝数量
	int residue = size & INT_MUL_8_MASK; // 计算还剩多少字节
	unsigned long long* ud = (unsigned long long*)d;
	unsigned long long* us = (unsigned long long*)s;
	for (size_t i = 0; i < size8; i++) // 8字节批量拷贝
		ud[i] = us[i];
	d += front;
	s += front;
	for (int i = 0; i < residue; i++)
		d[i] = s[i];
}

