#include "config.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <string>

namespace testapp {

namespace {

std::string MakeLowerAscii(
    std::string value)
{
    std::transform(
        value.begin(),
        value.end(),
        value.begin(),
        [](unsigned char ch) {
            return (char)std::tolower(ch);
        });
    return value;
}

bool ParseUnsigned(
    const char* text,
    uint64_t* value)
{
    char* end;
    uint64_t parsed;

    end = nullptr;
    parsed = _strtoui64(text, &end, 0);
    if ((end == text) || (*end != '\0')) {
        return false;
    }

    *value = parsed;
    return true;
}

} // namespace

std::wstring MediaModeToText(
    MediaMode mode)
{
    switch (mode) {
    case MediaMode::Auto:
        return L"auto";
    case MediaMode::Dense:
        return L"dense";
    case MediaMode::Sparse:
        return L"sparse";
    default:
        return L"unknown";
    }
}

bool TryParseMediaMode(
    const std::string& token,
    MediaMode* mode)
{
    const std::string lowered = MakeLowerAscii(token);

    if (lowered == "auto") {
        *mode = MediaMode::Auto;
        return true;
    }
    if (lowered == "dense") {
        *mode = MediaMode::Dense;
        return true;
    }
    if (lowered == "sparse") {
        *mode = MediaMode::Sparse;
        return true;
    }

    return false;
}

bool TryParseReadOnlyToken(
    const std::string& token,
    bool* read_only)
{
    const std::string lowered = MakeLowerAscii(token);

    if (lowered == "false") {
        *read_only = false;
        return true;
    }
    if (lowered == "true") {
        *read_only = true;
        return true;
    }

    return false;
}

MediaMode ResolveMediaMode(
    MediaMode requested_mode,
    uint64_t disk_size_bytes)
{
    if (requested_mode == MediaMode::Auto) {
        return disk_size_bytes <= kMaxDenseMediaBytes ? MediaMode::Dense : MediaMode::Sparse;
    }

    return requested_mode;
}

bool ParseTargetToken(
    const std::string& token,
    ULONG* target_id)
{
    uint64_t value;

    value = 0;
    if (!ParseUnsigned(token.c_str(), &value) || (value > YUMEDISK_MAX_USABLE_TARGET_ID)) {
        return false;
    }

    *target_id = (ULONG)value;
    return true;
}

void PrintUsage()
{
    std::cout
        << "cpp-cli [--queue-depth N] [--slot-bytes BYTES] [--sector-size BYTES]\n"
        << "\n"
        << "defaults:\n"
        << "  queue-depth = " << kDefaultQueueDepth << "\n"
        << "  slot-bytes  = " << kDefaultWriteSlotBytes << "\n"
        << "  sector-size = " << kDefaultSectorSize << "\n"
        << "  queue-depth max = " << kMaxSlotEngineQueueDepth << "\n"
        << "  dense media limit = " << (kMaxDenseMediaBytes / (1024ull * 1024ull)) << " MiB\n"
        << "\n"
        << "runtime create syntax:\n"
        << "  ct <disk-size-mb> [auto|dense|sparse] [true|false] [target]\n"
        << "    true  = system read-only disk\n"
        << "    false = read-write disk\n";
}

ParseResult ParseArgs(
    int argc,
    char** argv,
    AppConfig* config)
{
    int index;

    for (index = 1; index < argc; ++index) {
        const std::string arg = argv[index];
        uint64_t value;
        auto next_value = [&](uint64_t* out) -> bool {
            if ((index + 1) >= argc) {
                return false;
            }
            index += 1;
            return ParseUnsigned(argv[index], out);
        };

        value = 0;
        if ((arg == "--help") || (arg == "-h")) {
            return ParseResult::Help;
        }
        if (arg == "--queue-depth") {
            if (!next_value(&value) || (value == 0) ||
                (value > std::numeric_limits<size_t>::max()) ||
                (value > kMaxSlotEngineQueueDepth)) {
                return ParseResult::Error;
            }
            config->QueueDepth = (size_t)value;
            continue;
        }
        if (arg == "--slot-bytes") {
            if (!next_value(&value) || (value == 0) ||
                (value > std::numeric_limits<size_t>::max())) {
                return ParseResult::Error;
            }
            config->WriteSlotBytes = (size_t)value;
            continue;
        }
        if (arg == "--sector-size") {
            if (!next_value(&value) || (value == 0) ||
                (value > std::numeric_limits<ULONG>::max())) {
                return ParseResult::Error;
            }
            config->SectorSize = (ULONG)value;
            continue;
        }

        return ParseResult::Error;
    }

    if ((config->QueueDepth == 0) ||
        (config->WriteSlotBytes < config->SectorSize) ||
        ((config->WriteSlotBytes % config->SectorSize) != 0)) {
        return ParseResult::Error;
    }

    return ParseResult::Ok;
}

bool ParseCreateDiskCommand(
    const AppConfig& config,
    const std::vector<std::string>& tokens,
    CreateDiskRequest* request,
    std::wstring* error_text)
{
    uint64_t disk_size_mb;
    uint64_t disk_size_bytes;
    size_t index;
    bool mode_seen;
    bool read_only_seen;
    bool target_seen;

    if ((request == nullptr) || (error_text == nullptr)) {
        return false;
    }

    if (tokens.empty()) {
        *error_text = L"ct requires <disk-size-mb> [auto|dense|sparse] [true|false] [target]";
        return false;
    }

    disk_size_mb = 0;
    if (!ParseUnsigned(tokens[0].c_str(), &disk_size_mb) || (disk_size_mb == 0)) {
        *error_text = L"invalid disk size mb";
        return false;
    }

    disk_size_bytes = disk_size_mb * 1024ull * 1024ull;
    if ((disk_size_bytes / (1024ull * 1024ull)) != disk_size_mb) {
        *error_text = L"disk size overflow";
        return false;
    }

    request->DiskSizeBytes = disk_size_bytes;
    request->RequestedMode = MediaMode::Auto;
    request->TargetId = YUMEDISK_MAX_TARGETS;
    request->ReadOnly = false;
    mode_seen = false;
    read_only_seen = false;
    target_seen = false;

    for (index = 1; index < tokens.size(); ++index) {
        MediaMode parsed_mode;
        bool parsed_read_only;
        ULONG parsed_target;

        if (TryParseMediaMode(tokens[index], &parsed_mode)) {
            if (mode_seen) {
                *error_text = L"duplicate media mode";
                return false;
            }
            request->RequestedMode = parsed_mode;
            mode_seen = true;
            continue;
        }

        if (TryParseReadOnlyToken(tokens[index], &parsed_read_only)) {
            if (read_only_seen) {
                *error_text = L"duplicate readOnly";
                return false;
            }
            request->ReadOnly = parsed_read_only;
            read_only_seen = true;
            continue;
        }

        parsed_target = 0;
        if (ParseTargetToken(tokens[index], &parsed_target)) {
            if (target_seen) {
                *error_text = L"duplicate target";
                return false;
            }
            request->TargetId = parsed_target;
            target_seen = true;
            continue;
        }

        *error_text = L"invalid ct argument";
        return false;
    }

    if ((disk_size_bytes % config.SectorSize) != 0) {
        *error_text = L"disk size must align to sector size";
        return false;
    }

    if ((ResolveMediaMode(request->RequestedMode, disk_size_bytes) == MediaMode::Dense) &&
        (disk_size_bytes > kMaxDenseMediaBytes)) {
        *error_text = L"dense media requires disk size <= 1024 MiB";
        return false;
    }

    return true;
}

} // namespace testapp
