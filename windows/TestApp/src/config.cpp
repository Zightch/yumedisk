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
        << "TestApp [--queue-depth N] [--slot-bytes BYTES] [--disk-size-mb MB]\n"
        << "        [--sector-size BYTES] [--target ID] [--media auto|dense|sparse]\n"
        << "\n"
        << "defaults:\n"
        << "  queue-depth = " << kDefaultQueueDepth << "\n"
        << "  slot-bytes  = " << kDefaultWriteSlotBytes << "\n"
        << "  disk-size-mb = " << (kDefaultDiskSizeBytes / (1024ull * 1024ull)) << "\n"
        << "  sector-size = " << kDefaultSectorSize << "\n"
        << "  target      = " << kDefaultTargetId << "\n"
        << "  media       = auto\n"
        << "  queue-depth max = " << kMaxSlotEngineQueueDepth << "\n"
        << "  dense media limit = " << (kMaxDenseMediaBytes / (1024ull * 1024ull)) << " MiB\n";
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
        if (arg == "--disk-size-mb") {
            if (!next_value(&value) || (value == 0)) {
                return ParseResult::Error;
            }
            config->DiskSizeBytes = value * 1024ull * 1024ull;
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
        if (arg == "--target") {
            if (!next_value(&value) || (value > YUMEDISK_MAX_USABLE_TARGET_ID)) {
                return ParseResult::Error;
            }
            config->TargetId = (ULONG)value;
            continue;
        }
        if (arg == "--media") {
            MediaMode mode;

            if ((index + 1) >= argc) {
                return ParseResult::Error;
            }
            index += 1;
            if (!TryParseMediaMode(argv[index], &mode)) {
                return ParseResult::Error;
            }
            config->DefaultMediaMode = mode;
            continue;
        }

        return ParseResult::Error;
    }

    if ((config->QueueDepth == 0) ||
        (config->WriteSlotBytes < config->SectorSize) ||
        ((config->WriteSlotBytes % config->SectorSize) != 0) ||
        (config->DiskSizeBytes == 0) ||
        ((config->DiskSizeBytes % config->SectorSize) != 0)) {
        return ParseResult::Error;
    }

    if ((ResolveMediaMode(config->DefaultMediaMode, config->DiskSizeBytes) == MediaMode::Dense) &&
        (config->DiskSizeBytes > kMaxDenseMediaBytes)) {
        return ParseResult::Error;
    }

    return ParseResult::Ok;
}

} // namespace testapp
