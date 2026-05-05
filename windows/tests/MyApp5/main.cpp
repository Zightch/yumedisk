#include <Windows.h>

#include <cstdlib>
#include <cstring>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include "utils.h"
#include "yumedisk_proto.h"

static const char* kHelpText =
    "available commands:\n"
    "  help\n"
    "  query\n"
    "  create [target=<id>|<id>] [sectors=<count>|sectorCount=<count>|<count>] [sectorSize=<bytes>]\n"
    "  remove [target=<id>|<id>]\n"
    "  removeall [close]\n"
    "  exit\n";

static const LONG kProtocolSuccess = 0;

struct ControlContext {
    HANDLE controlFile{INVALID_HANDLE_VALUE};
    unsigned long long sessionId{0};
};

static std::vector<unsigned char> MakeMessageBuffer(ULONG command, ULONG payloadLength, ULONG capacityPayloadLength = 0) {
    if (capacityPayloadLength < payloadLength) {
        capacityPayloadLength = payloadLength;
    }

    std::vector<unsigned char> buffer(YUMEDISK_MESSAGE_BASE_SIZE + capacityPayloadLength, 0);
    auto* message = reinterpret_cast<PYUMEDISK_MESSAGE>(buffer.data());
    message->Header.Size = static_cast<ULONG>(buffer.size());
    message->Header.Version = YUMEDISK_PROTOCOL_VERSION;
    message->Header.Command = command;
    message->Header.PayloadLength = payloadLength;
    return buffer;
}

static bool SendCommand(HANDLE file, std::vector<unsigned char>& buffer, DWORD* bytesReturned = nullptr, bool verbose = true) {
    DWORD localBytesReturned = 0;
    DWORD overlappedBytesReturned = 0;
    OVERLAPPED overlapped{};
    BOOL ok = FALSE;

    overlapped.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (overlapped.hEvent == nullptr) {
        if (verbose) {
            std::cout << "CreateEvent failed, error=" << GetLastError() << std::endl;
        }
        return false;
    }

    ok = DeviceIoControl(
        file,
        IOCTL_YUMEDISK_APP_COMMAND,
        nullptr,
        0,
        buffer.data(),
        static_cast<DWORD>(buffer.size()),
        &overlappedBytesReturned,
        &overlapped
    );
    if (!ok && GetLastError() == ERROR_IO_PENDING) {
        if (WaitForSingleObject(overlapped.hEvent, INFINITE) != WAIT_OBJECT_0) {
            if (verbose) {
                std::cout << "WaitForSingleObject failed, error=" << GetLastError() << std::endl;
            }
            CloseHandle(overlapped.hEvent);
            return false;
        }

        ok = GetOverlappedResult(file, &overlapped, &overlappedBytesReturned, FALSE);
    }

    if (!ok) {
        if (verbose) {
            std::cout << "DeviceIoControl failed, error=" << GetLastError() << std::endl;
        }
        CloseHandle(overlapped.hEvent);
        return false;
    }

    if (bytesReturned != nullptr) {
        *bytesReturned = overlappedBytesReturned;
    } else {
        localBytesReturned = overlappedBytesReturned;
        (void)localBytesReturned;
    }

    CloseHandle(overlapped.hEvent);
    return true;
}

static std::vector<std::string> SplitArgs(const std::string& line) {
    std::vector<std::string> args;
    std::istringstream input(line);
    std::string token;

    while (input >> token) {
        args.push_back(token);
    }

    return args;
}

static bool ParseUnsigned64(const std::string& text, unsigned long long* value) {
    char* end = nullptr;
    unsigned long long parsed = _strtoui64(text.c_str(), &end, 0);
    if (end == text.c_str() || *end != '\0') {
        return false;
    }

    *value = parsed;
    return true;
}

static bool TryParseKeyValue(const std::string& token, const char* key, unsigned long long* value) {
    std::string prefix = std::string(key) + "=";
    if (token.rfind(prefix, 0) != 0) {
        return false;
    }

    return ParseUnsigned64(token.substr(prefix.size()), value);
}

static void PrintProtocolFailure(const char* commandName, const YUMEDISK_HEADER& header) {
    std::cout << commandName << " status=0x" << std::hex << header.Status << std::dec
              << ", session=" << header.SessionId
              << std::endl;
}

static void UpdateSessionId(ControlContext* context, const YUMEDISK_HEADER& header) {
    if (context != nullptr && header.SessionId != 0) {
        context->sessionId = header.SessionId;
    }
}

static bool EnsureSessionId(ControlContext* context) {
    auto buffer = MakeMessageBuffer(YumeDiskCommandQueryInfo, 0, sizeof(YUMEDISK_QUERY_INFO));
    auto* message = reinterpret_cast<PYUMEDISK_MESSAGE>(buffer.data());

    if (context->sessionId != 0) {
        return true;
    }

    if (!SendCommand(context->controlFile, buffer, nullptr, true)) {
        return false;
    }
    if (message->Header.Status != kProtocolSuccess || message->Header.SessionId == 0) {
        PrintProtocolFailure("QUERY_INFO", message->Header);
        return false;
    }

    UpdateSessionId(context, message->Header);
    return true;
}

static bool AttachSession(ControlContext* context, PYUMEDISK_MESSAGE message) {
    if (!EnsureSessionId(context)) {
        return false;
    }

    message->Header.SessionId = context->sessionId;
    return true;
}

static int RunQuery(ControlContext* context) {
    auto buffer = MakeMessageBuffer(YumeDiskCommandQueryInfo, 0, sizeof(YUMEDISK_QUERY_INFO));
    auto* message = reinterpret_cast<PYUMEDISK_MESSAGE>(buffer.data());

    if (!SendCommand(context->controlFile, buffer)) {
        return 1;
    }

    if (message->Header.Status != kProtocolSuccess || message->Header.PayloadLength < sizeof(YUMEDISK_QUERY_INFO)) {
        PrintProtocolFailure("QUERY_INFO", message->Header);
        return 1;
    }

    UpdateSessionId(context, message->Header);
    auto* info = reinterpret_cast<PYUMEDISK_QUERY_INFO>(message->Payload);
    std::wcout << L"service=" << info->ServiceName << std::endl;
    std::cout << "protocol=" << info->ProtocolVersion
              << ", maxTargets=" << info->MaxTargets
              << ", features=0x" << std::hex << info->Features << std::dec
              << ", session=" << message->Header.SessionId
              << std::endl;
    return 0;
}

static int RunCreate(ControlContext* context, const std::vector<std::string>& args) {
    unsigned long long targetId = YUMEDISK_MIN_TARGET_ID;
    unsigned long long sectorCount = 0x200000ull;
    unsigned long long sectorSize = YUMEDISK_DEFAULT_SECTOR_SIZE;
    bool targetSeen = false;
    bool sectorCountSeen = false;

    for (size_t i = 1; i < args.size(); ++i) {
        unsigned long long value = 0;

        if (TryParseKeyValue(args[i], "target", &value)) {
            targetId = value;
            targetSeen = true;
            continue;
        }
        if (TryParseKeyValue(args[i], "sectors", &value) || TryParseKeyValue(args[i], "sectorCount", &value)) {
            sectorCount = value;
            sectorCountSeen = true;
            continue;
        }
        if (TryParseKeyValue(args[i], "sectorSize", &value)) {
            sectorSize = value;
            continue;
        }

        if (!targetSeen && ParseUnsigned64(args[i], &value)) {
            targetId = value;
            targetSeen = true;
            continue;
        }
        if (!sectorCountSeen && ParseUnsigned64(args[i], &value)) {
            sectorCount = value;
            sectorCountSeen = true;
            continue;
        }

        std::cout << "invalid create argument: " << args[i] << std::endl;
        return 1;
    }

    auto buffer = MakeMessageBuffer(YumeDiskCommandCreateDisk, sizeof(YUMEDISK_CREATE_DISK));
    auto* message = reinterpret_cast<PYUMEDISK_MESSAGE>(buffer.data());
    auto* request = reinterpret_cast<PYUMEDISK_CREATE_DISK>(message->Payload);

    if (!AttachSession(context, message)) {
        return 1;
    }
    if (targetId < YUMEDISK_MIN_TARGET_ID || targetId > YUMEDISK_MAX_USABLE_TARGET_ID) {
        std::cout << "invalid target, usable range is "
                  << YUMEDISK_MIN_TARGET_ID
                  << ".."
                  << YUMEDISK_MAX_USABLE_TARGET_ID
                  << std::endl;
        return 1;
    }

    request->TargetId = static_cast<ULONG>(targetId);
    request->SectorSize = static_cast<ULONG>(sectorSize);
    request->SectorCount = static_cast<ULONGLONG>(sectorCount);
    request->DiskSizeBytes = static_cast<ULONGLONG>(sectorCount * sectorSize);

    if (!SendCommand(context->controlFile, buffer)) {
        return 1;
    }

    UpdateSessionId(context, message->Header);
    std::cout << "CREATE_DISK status=0x" << std::hex << message->Header.Status << std::dec
              << ", target=" << request->TargetId
              << ", sectors=" << request->SectorCount
              << ", sectorSize=" << request->SectorSize
              << ", session=" << message->Header.SessionId
              << std::endl;
    return message->Header.Status == kProtocolSuccess ? 0 : 1;
}

static int RunRemove(ControlContext* context, const std::vector<std::string>& args) {
    unsigned long long targetId = YUMEDISK_MIN_TARGET_ID;
    bool targetSeen = false;

    for (size_t i = 1; i < args.size(); ++i) {
        unsigned long long value = 0;

        if (TryParseKeyValue(args[i], "target", &value)) {
            targetId = value;
            targetSeen = true;
            continue;
        }
        if (!targetSeen && ParseUnsigned64(args[i], &value)) {
            targetId = value;
            targetSeen = true;
            continue;
        }

        std::cout << "invalid remove argument: " << args[i] << std::endl;
        return 1;
    }

    auto buffer = MakeMessageBuffer(YumeDiskCommandRemoveDisk, sizeof(YUMEDISK_REMOVE_DISK));
    auto* message = reinterpret_cast<PYUMEDISK_MESSAGE>(buffer.data());
    auto* request = reinterpret_cast<PYUMEDISK_REMOVE_DISK>(message->Payload);

    if (!AttachSession(context, message)) {
        return 1;
    }
    if (targetId < YUMEDISK_MIN_TARGET_ID || targetId > YUMEDISK_MAX_USABLE_TARGET_ID) {
        std::cout << "invalid target, usable range is "
                  << YUMEDISK_MIN_TARGET_ID
                  << ".."
                  << YUMEDISK_MAX_USABLE_TARGET_ID
                  << std::endl;
        return 1;
    }

    request->TargetId = static_cast<ULONG>(targetId);

    if (!SendCommand(context->controlFile, buffer)) {
        return 1;
    }

    UpdateSessionId(context, message->Header);
    std::cout << "REMOVE_DISK status=0x" << std::hex << message->Header.Status << std::dec
              << ", target=" << request->TargetId
              << ", session=" << message->Header.SessionId
              << std::endl;
    return message->Header.Status == kProtocolSuccess ? 0 : 1;
}

static int RunRemoveAll(ControlContext* context, const std::vector<std::string>& args) {
    auto buffer = MakeMessageBuffer(YumeDiskCommandRemoveAllDisks, 0);
    auto* message = reinterpret_cast<PYUMEDISK_MESSAGE>(buffer.data());
    bool closeSession = false;

    for (size_t i = 1; i < args.size(); ++i) {
        if (args[i] == "close") {
            closeSession = true;
            continue;
        }

        std::cout << "invalid removeall argument: " << args[i] << std::endl;
        return 1;
    }

    if (!AttachSession(context, message)) {
        return 1;
    }

    if (closeSession) {
        message->Header.Flags = YUMEDISK_SESSION_CLOSE_FLAG;
    }

    if (!SendCommand(context->controlFile, buffer)) {
        return 1;
    }

    UpdateSessionId(context, message->Header);
    std::cout << "REMOVE_ALL_DISKS status=0x" << std::hex << message->Header.Status << std::dec
              << ", session=" << message->Header.SessionId
              << std::endl;
    return message->Header.Status == kProtocolSuccess ? 0 : 1;
}

static int ExecuteCommand(ControlContext* context, const std::string& line, bool* shouldExit) {
    auto args = SplitArgs(line);
    if (args.empty()) {
        return 0;
    }

    const std::string& command = args[0];
    if (command == "help") {
        std::cout << kHelpText;
        return 0;
    }
    if (command == "query") {
        return RunQuery(context);
    }
    if (command == "create") {
        return RunCreate(context, args);
    }
    if (command == "remove") {
        return RunRemove(context, args);
    }
    if (command == "removeall") {
        return RunRemoveAll(context, args);
    }
    if (command == "exit" || command == "quit") {
        *shouldExit = true;
        return 0;
    }

    std::cout << "unknown command: " << command << std::endl;
    return 1;
}

int main(int argc, char** argv) {
    ControlContext context{};
    HANDLE file = OpenFirstDeviceInterface(&GUID_YUMEDISK_CONTROL, true);
    if (file == INVALID_HANDLE_VALUE) {
        std::cout << "open control device failed, error=" << GetLastError() << std::endl;
        return 1;
    }

    context.controlFile = file;

    int exitCode = 0;
    if (argc > 1) {
        std::ostringstream singleCommand;
        for (int i = 1; i < argc; ++i) {
            if (i != 1) {
                singleCommand << ' ';
            }
            singleCommand << argv[i];
        }

        bool shouldExit = false;
        exitCode = ExecuteCommand(&context, singleCommand.str(), &shouldExit);
        CloseHandle(file);
        return exitCode;
    }

    std::cout << "YumeDisk control session opened" << std::endl;
    std::cout << kHelpText;

    for (;;) {
        std::string line;
        bool shouldExit = false;

        std::cout << "\n>";
        if (!std::getline(std::cin, line)) {
            break;
        }

        exitCode = ExecuteCommand(&context, line, &shouldExit);
        if (shouldExit) {
            break;
        }
    }

    CloseHandle(file);
    return exitCode;
}
