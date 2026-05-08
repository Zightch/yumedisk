#pragma once

#define NOMINMAX

#include <Windows.h>
#include <json/json.h>

#include <array>
#include <filesystem>
#include <initializer_list>
#include <string>
#include <utility>
#include <vector>

namespace devtidy {

namespace fs = std::filesystem;

struct DeviceSpec {
    const wchar_t* name;
    const wchar_t* hardwareId;
    const wchar_t* className;
    const GUID* classGuid;
    const wchar_t* packageDirName;
};

struct DeviceInstance {
    std::wstring instanceId;
    ULONG status;
    ULONG problem;
    bool hasStatus;
};

struct CertificateSpec {
    fs::path path;
    std::vector<BYTE> encoded;
    std::wstring subject;
    std::wstring thumbprint;
};

enum class CertificateMode {
    SelfSigned,
    Release,
    Mixed
};

enum class RunMode {
    Install,
    Uninstall
};

extern const std::array<DeviceSpec, 2> kDeviceSpecs;

std::string WideToUtf8(const std::wstring& value);
std::string WideToUtf8(const wchar_t* value);

Json::Value JsonText(const std::string& value);
Json::Value JsonText(const std::wstring& value);
Json::Value JsonText(const wchar_t* value);
Json::Value JsonTextArray(const std::vector<std::wstring>& values);
Json::Value JsonUint(DWORD value);
Json::Value JsonSize(size_t value);
Json::Value JsonObject(
    std::initializer_list<std::pair<const char*, Json::Value>> fields = {});

void WriteInfoEvent(const char* event, const Json::Value& data = JsonObject());
void WriteInfoDeviceEvent(
    const char* event,
    const wchar_t* device,
    const Json::Value& data = JsonObject());
void WriteErrorEvent(const char* event, const Json::Value& data = JsonObject());
void WriteErrorDeviceEvent(
    const char* event,
    const wchar_t* device,
    const Json::Value& data = JsonObject());

std::wstring GetLastErrorMessage(DWORD error);
fs::path GetPackageRootFromExecutable();
bool EqualInsensitive(const std::wstring& left, const std::wstring& right);
bool MultiSzContains(const std::vector<wchar_t>& buffer, const std::wstring& value);
std::wstring BytesToHex(const BYTE* data, size_t length);

}  // namespace devtidy
