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

void* malloc(size_t size) {
	// NT_ASSERT(KeGetCurrentIrql() == PASSIVE_LEVEL);
	return ExAllocatePool2(POOL_FLAG_NON_PAGED, size, MEM_TAG);
}

void free(void* p) {
	// NT_ASSERT(KeGetCurrentIrql() == PASSIVE_LEVEL);
	ExFreePoolWithTag(p, MEM_TAG);
}
