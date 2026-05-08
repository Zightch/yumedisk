#include "common.h"

#include <devguid.h>

#include <cwctype>
#include <iostream>

namespace devtidy {

const std::array<DeviceSpec, 2> kDeviceSpecs = {{
    {
        L"YumeDiskSCSI",
        L"Root\\YumeDiskSCSI",
        L"SCSIAdapter",
        &GUID_DEVCLASS_SCSIADAPTER,
        L"YumeDiskSCSI"
    },
    {
        L"YumeDiskKMDF",
        L"Root\\YumeDiskKMDF",
        L"System",
        &GUID_DEVCLASS_SYSTEM,
        L"YumeDiskKMDF"
    }
}};

namespace {

const char* kOutputSchema = "v1";

void WriteEvent(
    const char* level,
    const char* event,
    const wchar_t* device,
    const Json::Value& data
) {
    Json::Value root(Json::objectValue);
    root["schema"] = kOutputSchema;
    root["level"] = level;
    root["event"] = event;
    root["device"] = (device != nullptr) ? JsonText(device) : Json::Value(Json::nullValue);
    root["data"] = data;

    Json::StreamWriterBuilder builder;
    builder["indentation"] = "";
    builder["commentStyle"] = "None";
    builder["enableYAMLCompatibility"] = false;

    std::cout << Json::writeString(builder, root) << std::endl;
}

fs::path GetExecutablePath() {
    std::wstring buffer(MAX_PATH, L'\0');

    for (;;) {
        const DWORD length = GetModuleFileNameW(
            nullptr,
            buffer.data(),
            static_cast<DWORD>(buffer.size()));
        if (length == 0) {
            return {};
        }
        if (length < buffer.size() - 1) {
            buffer.resize(length);
            return fs::path(buffer);
        }

        buffer.resize(buffer.size() * 2);
    }
}

}  // namespace

std::string WideToUtf8(const std::wstring& value) {
    if (value.empty()) {
        return {};
    }

    const int size = WideCharToMultiByte(
        CP_UTF8,
        0,
        value.data(),
        static_cast<int>(value.size()),
        nullptr,
        0,
        nullptr,
        nullptr);
    if (size <= 0) {
        return {};
    }

    std::string result(static_cast<size_t>(size), '\0');
    WideCharToMultiByte(
        CP_UTF8,
        0,
        value.data(),
        static_cast<int>(value.size()),
        result.data(),
        size,
        nullptr,
        nullptr);
    return result;
}

std::string WideToUtf8(const wchar_t* value) {
    if (value == nullptr) {
        return {};
    }

    return WideToUtf8(std::wstring(value));
}

Json::Value JsonText(const std::string& value) {
    return Json::Value(value);
}

Json::Value JsonText(const std::wstring& value) {
    return Json::Value(WideToUtf8(value));
}

Json::Value JsonText(const wchar_t* value) {
    return Json::Value(WideToUtf8(value));
}

Json::Value JsonTextArray(const std::vector<std::wstring>& values) {
    Json::Value array(Json::arrayValue);
    for (const std::wstring& value : values) {
        array.append(WideToUtf8(value));
    }

    return array;
}

Json::Value JsonUint(DWORD value) {
    return Json::Value(static_cast<Json::UInt>(value));
}

Json::Value JsonSize(size_t value) {
    return Json::Value(static_cast<Json::UInt64>(value));
}

Json::Value JsonObject(
    std::initializer_list<std::pair<const char*, Json::Value>> fields
) {
    Json::Value object(Json::objectValue);
    for (const auto& field : fields) {
        object[field.first] = field.second;
    }

    return object;
}

void WriteInfoEvent(const char* event, const Json::Value& data) {
    WriteEvent("info", event, nullptr, data);
}

void WriteInfoDeviceEvent(
    const char* event,
    const wchar_t* device,
    const Json::Value& data
) {
    WriteEvent("info", event, device, data);
}

void WriteErrorEvent(const char* event, const Json::Value& data) {
    WriteEvent("error", event, nullptr, data);
}

void WriteErrorDeviceEvent(
    const char* event,
    const wchar_t* device,
    const Json::Value& data
) {
    WriteEvent("error", event, device, data);
}

std::wstring GetLastErrorMessage(DWORD error) {
    wchar_t* buffer = nullptr;
    const DWORD length = FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER |
            FORMAT_MESSAGE_FROM_SYSTEM |
            FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr,
        error,
        0,
        reinterpret_cast<LPWSTR>(&buffer),
        0,
        nullptr);

    std::wstring message;
    if (length != 0 && buffer != nullptr) {
        message.assign(buffer, length);
        while (!message.empty() &&
               (message.back() == L'\r' || message.back() == L'\n' || message.back() == L' ')) {
            message.pop_back();
        }
    } else {
        message = L"unknown error";
    }

    if (buffer != nullptr) {
        LocalFree(buffer);
    }

    return message;
}

fs::path GetPackageRootFromExecutable() {
    const fs::path exePath = GetExecutablePath();
    if (exePath.empty()) {
        return {};
    }

    return exePath.parent_path();
}

bool EqualInsensitive(const std::wstring& left, const std::wstring& right) {
    if (left.size() != right.size()) {
        return false;
    }

    for (size_t index = 0; index < left.size(); ++index) {
        if (towlower(left[index]) != towlower(right[index])) {
            return false;
        }
    }

    return true;
}

bool MultiSzContains(const std::vector<wchar_t>& buffer, const std::wstring& value) {
    size_t offset = 0;
    while (offset < buffer.size() && buffer[offset] != L'\0') {
        const std::wstring item(&buffer[offset]);
        if (EqualInsensitive(item, value)) {
            return true;
        }

        offset += item.size() + 1;
    }

    return false;
}

std::wstring BytesToHex(const BYTE* data, size_t length) {
    static const wchar_t digits[] = L"0123456789ABCDEF";

    std::wstring value;
    value.reserve(length * 2);
    for (size_t index = 0; index < length; ++index) {
        value.push_back(digits[(data[index] >> 4) & 0x0F]);
        value.push_back(digits[data[index] & 0x0F]);
    }

    return value;
}

}  // namespace devtidy
