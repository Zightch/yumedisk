#ifndef _UTILS_H
#define _UTILS_H

#include <ntddk.h>

RTL_GENERIC_COMPARE_RESULTS AvlCmp(PRTL_AVL_TABLE Table, PVOID FirstStruct, PVOID SecondStruct);
PVOID AvlAlloc(PRTL_AVL_TABLE Table, CLONG  ByteSize);
void AvlFree(PRTL_AVL_TABLE Table, PVOID  Buffer);
void FastMemcpy(char* d, char* s, size_t size);

#endif // !_UTILS_H
