#pragma once

#include <string>

#include "types.h"

namespace testapp {

std::wstring FormatStatusHex(AK_STATUS status);
std::wstring FormatVersionBe(UINT32 version_be);

void LogWide(
    BackendContext* context,
    const std::wstring& text);

VOID AK_CALL AppKernelLogCallback(
    void* log_ctx,
    INT level,
    const char* text);

void PrintBackendStats(BackendContext* context);
void PrintDebugSnapshot(BackendContext* context, const wchar_t* reason);

void RunEventLoop(BackendContext* context);
void RunCommandLoop(BackendContext* context);

bool RemoveAllManagedDisks(
    BackendContext* context,
    bool closing);

} // namespace testapp
