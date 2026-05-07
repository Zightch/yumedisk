#pragma once

#include <string>

#include "types.h"

namespace testapp {

std::wstring MediaModeToText(MediaMode mode);
bool TryParseMediaMode(const std::string& token, MediaMode* mode);
MediaMode ResolveMediaMode(MediaMode requested_mode, uint64_t disk_size_bytes);
bool ParseTargetToken(const std::string& token, ULONG* target_id);

void PrintUsage();
ParseResult ParseArgs(int argc, char** argv, AppConfig* config);

} // namespace testapp
