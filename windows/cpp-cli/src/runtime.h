#pragma once

#include <string>

#include "types.h"

namespace testapp {

std::wstring FormatVersionBe(UINT32 versionBe);

void PrintBackendStats(CliContext* context);
void PrintDebugSnapshot(
    CliContext* context,
    const wchar_t* reason);

void RunCommandLoop(CliContext* context);

bool RemoveAllManagedDisks(
    CliContext* context,
    bool closing);

} // namespace testapp
