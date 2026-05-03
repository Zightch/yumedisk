#ifndef _UTILS_H
#define _UTILS_H

#include <ntddk.h>

void* malloc(size_t size);
void free(void* p);

RTL_GENERIC_COMPARE_RESULTS AvlCmp(PRTL_AVL_TABLE Table, PVOID FirstStruct, PVOID SecondStruct);
PVOID AvlAlloc(PRTL_AVL_TABLE Table, CLONG  ByteSize);
void AvlFree(PRTL_AVL_TABLE Table, PVOID  Buffer);

#endif // !_UTILS_H
