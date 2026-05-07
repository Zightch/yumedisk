#define NOMINMAX

#include <Windows.h>
#include <cfgmgr32.h>
#include <devguid.h>
#include <json/json.h>
#include <setupapi.h>

#include <algorithm>
#include <filesystem>
#include <initializer_list>
#include <iostream>
#include <string>
#include <vector>

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

struct PackageState {
    bool alreadyPresent;
    std::wstring publishedInf;
};

enum class RunMode {
    Install,
    Uninstall
};

using DiUninstallDeviceFn = BOOL(WINAPI*)(HWND, HDEVINFO, PSP_DEVINFO_DATA, DWORD, PBOOL);
using DiUninstallDriverWFn = BOOL(WINAPI*)(HWND, LPCWSTR, DWORD, PBOOL);

static const DeviceSpec kDeviceSpecs[] = {
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
};

static const char* kOutputSchema = "devtidy.v1";

static std::string WideToUtf8(const std::wstring& value) {
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

static std::string WideToUtf8(const wchar_t* value) {
    if (value == nullptr) {
        return {};
    }

    return WideToUtf8(std::wstring(value));
}

static Json::Value JsonText(const std::string& value) {
    return Json::Value(value);
}

static Json::Value JsonText(const std::wstring& value) {
    return Json::Value(WideToUtf8(value));
}

static Json::Value JsonText(const wchar_t* value) {
    return Json::Value(WideToUtf8(value));
}

static Json::Value JsonUint(DWORD value) {
    return Json::Value(static_cast<Json::UInt>(value));
}

static Json::Value JsonSize(size_t value) {
    return Json::Value(static_cast<Json::UInt64>(value));
}

static Json::Value JsonObject(
    std::initializer_list<std::pair<const char*, Json::Value>> fields = {}
) {
    Json::Value object(Json::objectValue);
    for (const auto& field : fields) {
        object[field.first] = field.second;
    }

    return object;
}

static void WriteEvent(
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

static void WriteInfoEvent(const char* event, const Json::Value& data = JsonObject()) {
    WriteEvent("info", event, nullptr, data);
}

static void WriteInfoDeviceEvent(
    const char* event,
    const wchar_t* device,
    const Json::Value& data = JsonObject()
) {
    WriteEvent("info", event, device, data);
}

static void WriteErrorEvent(const char* event, const Json::Value& data = JsonObject()) {
    WriteEvent("error", event, nullptr, data);
}

static void WriteErrorDeviceEvent(
    const char* event,
    const wchar_t* device,
    const Json::Value& data = JsonObject()
) {
    WriteEvent("error", event, device, data);
}

static std::wstring GetLastErrorMessage(DWORD error) {
    wchar_t* buffer = nullptr;
    DWORD length = FormatMessageW(
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

static fs::path GetExecutablePath() {
    std::wstring buffer(MAX_PATH, L'\0');

    for (;;) {
        DWORD length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
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

static fs::path GetPackageRootFromExecutable() {
    const fs::path exePath = GetExecutablePath();
    if (exePath.empty()) {
        return {};
    }

    return exePath.parent_path();
}

static bool EqualInsensitive(const std::wstring& left, const std::wstring& right) {
    if (left.size() != right.size()) {
        return false;
    }

    for (size_t i = 0; i < left.size(); ++i) {
        if (towlower(left[i]) != towlower(right[i])) {
            return false;
        }
    }

    return true;
}

static bool MultiSzContains(const std::vector<wchar_t>& buffer, const std::wstring& value) {
    size_t offset = 0;
    while (offset < buffer.size() && buffer[offset] != L'\0') {
        std::wstring item(&buffer[offset]);
        if (EqualInsensitive(item, value)) {
            return true;
        }

        offset += item.size() + 1;
    }

    return false;
}

static std::vector<DeviceInstance> EnumerateDevicesByHardwareId(const DeviceSpec& spec) {
    std::vector<DeviceInstance> devices;
    HDEVINFO info = SetupDiGetClassDevsW(nullptr, nullptr, nullptr, DIGCF_ALLCLASSES);
    if (info == INVALID_HANDLE_VALUE) {
        return devices;
    }

    for (DWORD index = 0;; ++index) {
        SP_DEVINFO_DATA devInfoData{};
        DWORD requiredSize = 0;
        DWORD regType = 0;

        devInfoData.cbSize = sizeof(devInfoData);
        if (!SetupDiEnumDeviceInfo(info, index, &devInfoData)) {
            if (GetLastError() == ERROR_NO_MORE_ITEMS) {
                break;
            }
            continue;
        }

        SetupDiGetDeviceRegistryPropertyW(
            info,
            &devInfoData,
            SPDRP_HARDWAREID,
            &regType,
            nullptr,
            0,
            &requiredSize);
        if (GetLastError() != ERROR_INSUFFICIENT_BUFFER || requiredSize == 0) {
            continue;
        }

        std::vector<wchar_t> hardwareIds((requiredSize / sizeof(wchar_t)) + 1, L'\0');
        if (!SetupDiGetDeviceRegistryPropertyW(
                info,
                &devInfoData,
                SPDRP_HARDWAREID,
                &regType,
                reinterpret_cast<PBYTE>(hardwareIds.data()),
                requiredSize,
                nullptr)) {
            continue;
        }

        if (!MultiSzContains(hardwareIds, spec.hardwareId)) {
            continue;
        }

        std::wstring instanceId(MAX_DEVICE_ID_LEN, L'\0');
        if (CM_Get_Device_IDW(devInfoData.DevInst, instanceId.data(), MAX_DEVICE_ID_LEN, 0) != CR_SUCCESS) {
            continue;
        }
        instanceId.resize(wcslen(instanceId.c_str()));

        DeviceInstance instance{};
        instance.instanceId = instanceId;
        instance.status = 0;
        instance.problem = 0;
        instance.hasStatus =
            CM_Get_DevNode_Status(&instance.status, &instance.problem, devInfoData.DevInst, 0) == CR_SUCCESS;
        devices.push_back(instance);
    }

    SetupDiDestroyDeviceInfoList(info);
    return devices;
}

static bool UninstallDeviceByInstanceId(
    const std::wstring& instanceId,
    bool* needReboot,
    std::wstring* errorMessage
) {
    HMODULE newdevModule = LoadLibraryW(L"newdev.dll");
    if (newdevModule == nullptr) {
        if (errorMessage != nullptr) {
            const DWORD error = GetLastError();
            *errorMessage = GetLastErrorMessage(error);
        }
        return false;
    }

    const auto uninstallDevice =
        reinterpret_cast<DiUninstallDeviceFn>(GetProcAddress(newdevModule, "DiUninstallDevice"));
    if (uninstallDevice == nullptr) {
        if (errorMessage != nullptr) {
            *errorMessage = L"DiUninstallDevice not available in newdev.dll";
        }
        FreeLibrary(newdevModule);
        return false;
    }

    HDEVINFO info = SetupDiGetClassDevsW(nullptr, nullptr, nullptr, DIGCF_ALLCLASSES);
    if (info == INVALID_HANDLE_VALUE) {
        if (errorMessage != nullptr) {
            const DWORD error = GetLastError();
            *errorMessage = GetLastErrorMessage(error);
        }
        FreeLibrary(newdevModule);
        return false;
    }

    bool removed = false;
    for (DWORD index = 0;; ++index) {
        SP_DEVINFO_DATA devInfoData{};
        std::wstring currentId(MAX_DEVICE_ID_LEN, L'\0');

        devInfoData.cbSize = sizeof(devInfoData);
        if (!SetupDiEnumDeviceInfo(info, index, &devInfoData)) {
            if (GetLastError() == ERROR_NO_MORE_ITEMS) {
                break;
            }
            continue;
        }

        if (CM_Get_Device_IDW(devInfoData.DevInst, currentId.data(), MAX_DEVICE_ID_LEN, 0) != CR_SUCCESS) {
            continue;
        }
        currentId.resize(wcslen(currentId.c_str()));
        if (!EqualInsensitive(currentId, instanceId)) {
            continue;
        }

        BOOL localNeedReboot = FALSE;
        if (!uninstallDevice(nullptr, info, &devInfoData, 0, &localNeedReboot)) {
            if (errorMessage != nullptr) {
                const DWORD error = GetLastError();
                *errorMessage = GetLastErrorMessage(error);
            }
            SetupDiDestroyDeviceInfoList(info);
            FreeLibrary(newdevModule);
            return false;
        }

        if (needReboot != nullptr && localNeedReboot != FALSE) {
            *needReboot = true;
        }
        removed = true;
        break;
    }

    SetupDiDestroyDeviceInfoList(info);
    FreeLibrary(newdevModule);
    if (!removed && errorMessage != nullptr) {
        *errorMessage = L"device instance not found";
    }
    return removed;
}

static int DeviceRank(const DeviceInstance& instance) {
    int rank = 0;

    if (instance.hasStatus && (instance.status & DN_STARTED) != 0) {
        rank += 4;
    }
    if (instance.hasStatus && (instance.status & DN_HAS_PROBLEM) == 0) {
        rank += 2;
    }
    if (instance.problem == 0) {
        rank += 1;
    }

    return rank;
}

static size_t ChoosePrimaryDeviceIndex(const std::vector<DeviceInstance>& devices) {
    size_t bestIndex = 0;

    for (size_t index = 1; index < devices.size(); ++index) {
        const int currentRank = DeviceRank(devices[index]);
        const int bestRank = DeviceRank(devices[bestIndex]);

        if (currentRank > bestRank) {
            bestIndex = index;
            continue;
        }
        if (currentRank == bestRank && devices[index].instanceId < devices[bestIndex].instanceId) {
            bestIndex = index;
        }
    }

    return bestIndex;
}

static bool TryFindSingleInfPath(
    const DeviceSpec& spec,
    const fs::path& packageRoot,
    fs::path* infPath
) {
    const fs::path packageDir = packageRoot / spec.packageDirName;
    std::error_code ec;

    if (!fs::exists(packageDir, ec) || !fs::is_directory(packageDir, ec)) {
        WriteErrorDeviceEvent(
            "package_dir_not_found",
            spec.name,
            JsonObject({
                {"package_dir", JsonText(packageDir.native())}
            }));
        return false;
    }

    std::vector<fs::path> infFiles;
    for (const fs::directory_entry& entry : fs::directory_iterator(packageDir, ec)) {
        if (ec) {
            break;
        }
        if (!entry.is_regular_file()) {
            continue;
        }
        if (EqualInsensitive(entry.path().extension().native(), L".inf")) {
            infFiles.push_back(entry.path());
        }
    }

    if (ec) {
        WriteErrorDeviceEvent(
            "package_dir_scan_failed",
            spec.name,
            JsonObject({
                {"package_dir", JsonText(packageDir.native())},
                {"error", JsonText(ec.message())}
            }));
        return false;
    }

    std::sort(infFiles.begin(), infFiles.end());
    if (infFiles.empty()) {
        WriteErrorDeviceEvent(
            "inf_not_found",
            spec.name,
            JsonObject({
                {"package_dir", JsonText(packageDir.native())}
            }));
        return false;
    }
    if (infFiles.size() > 1) {
        Json::Value found(Json::arrayValue);
        for (const fs::path& candidate : infFiles) {
            found.append(WideToUtf8(candidate.filename().native()));
        }

        WriteErrorDeviceEvent(
            "multiple_inf_found",
            spec.name,
            JsonObject({
                {"package_dir", JsonText(packageDir.native())},
                {"files", found}
            }));
        return false;
    }

    *infPath = infFiles.front();
    return true;
}

static bool EnsureDriverPackage(const DeviceSpec& spec, const fs::path& infPath, PackageState* state) {
    wchar_t publishedInf[MAX_PATH] = {};

    if (!SetupCopyOEMInfW(
            infPath.c_str(),
            nullptr,
            SPOST_PATH,
            SP_COPY_NOOVERWRITE,
            publishedInf,
            ARRAYSIZE(publishedInf),
            nullptr,
            nullptr)) {
        const DWORD error = GetLastError();
        if (error != ERROR_FILE_EXISTS) {
            WriteErrorDeviceEvent(
                "package_stage_failed",
                spec.name,
                JsonObject({
                    {"source_inf", JsonText(infPath.native())},
                    {"error_code", JsonUint(error)},
                    {"error", JsonText(GetLastErrorMessage(error))}
                }));
            return false;
        }

        if (state != nullptr) {
            state->alreadyPresent = true;
            state->publishedInf.clear();
        }

        WriteInfoDeviceEvent(
            "package_present",
            spec.name,
            JsonObject({
                {"source_inf", JsonText(infPath.native())}
            }));
        return true;
    }

    if (state != nullptr) {
        state->alreadyPresent = false;
        state->publishedInf = publishedInf;
    }

    WriteInfoDeviceEvent(
        "package_staged",
        spec.name,
        JsonObject({
            {"published_inf", JsonText(publishedInf)}
        }));
    return true;
}

static bool CreateRootDevice(const DeviceSpec& spec, std::wstring* createdInstanceId) {
    HDEVINFO info = SetupDiCreateDeviceInfoList(spec.classGuid, nullptr);
    if (info == INVALID_HANDLE_VALUE) {
        const DWORD error = GetLastError();
        WriteErrorDeviceEvent(
            "device_info_list_failed",
            spec.name,
            JsonObject({
                {"error_code", JsonUint(error)},
                {"error", JsonText(GetLastErrorMessage(error))}
            }));
        return false;
    }

    SP_DEVINFO_DATA devInfoData{};
    devInfoData.cbSize = sizeof(devInfoData);

    if (!SetupDiCreateDeviceInfoW(
            info,
            spec.className,
            spec.classGuid,
            spec.name,
            nullptr,
            DICD_GENERATE_ID,
            &devInfoData)) {
        const DWORD error = GetLastError();
        WriteErrorDeviceEvent(
            "device_create_failed",
            spec.name,
            JsonObject({
                {"error_code", JsonUint(error)},
                {"error", JsonText(GetLastErrorMessage(error))}
            }));
        SetupDiDestroyDeviceInfoList(info);
        return false;
    }

    std::wstring hardwareIds(spec.hardwareId);
    hardwareIds.push_back(L'\0');
    hardwareIds.push_back(L'\0');

    if (!SetupDiSetDeviceRegistryPropertyW(
            info,
            &devInfoData,
            SPDRP_HARDWAREID,
            reinterpret_cast<const BYTE*>(hardwareIds.c_str()),
            static_cast<DWORD>(hardwareIds.size() * sizeof(wchar_t)))) {
        const DWORD error = GetLastError();
        WriteErrorDeviceEvent(
            "device_hwid_failed",
            spec.name,
            JsonObject({
                {"error_code", JsonUint(error)},
                {"error", JsonText(GetLastErrorMessage(error))}
            }));
        SetupDiDestroyDeviceInfoList(info);
        return false;
    }

    if (!SetupDiCallClassInstaller(DIF_REGISTERDEVICE, info, &devInfoData)) {
        const DWORD error = GetLastError();
        WriteErrorDeviceEvent(
            "device_register_failed",
            spec.name,
            JsonObject({
                {"error_code", JsonUint(error)},
                {"error", JsonText(GetLastErrorMessage(error))}
            }));
        SetupDiDestroyDeviceInfoList(info);
        return false;
    }

    if (createdInstanceId != nullptr) {
        std::wstring instanceId(MAX_DEVICE_ID_LEN, L'\0');
        if (CM_Get_Device_IDW(devInfoData.DevInst, instanceId.data(), MAX_DEVICE_ID_LEN, 0) == CR_SUCCESS) {
            instanceId.resize(wcslen(instanceId.c_str()));
            *createdInstanceId = instanceId;
        } else {
            createdInstanceId->clear();
        }
    }

    SetupDiDestroyDeviceInfoList(info);
    return true;
}

static bool EnsureUniqueDevice(const DeviceSpec& spec) {
    std::vector<DeviceInstance> devices = EnumerateDevicesByHardwareId(spec);

    if (devices.empty()) {
        std::wstring createdInstanceId;
        if (!CreateRootDevice(spec, &createdInstanceId)) {
            return false;
        }

        WriteInfoDeviceEvent(
            "device_created",
            spec.name,
            JsonObject({
                {"instance_id", JsonText(createdInstanceId)},
                {"instance_count", JsonSize(1)},
                {"duplicate_count", JsonSize(0)}
            }));
        return true;
    }

    const size_t keepIndex = ChoosePrimaryDeviceIndex(devices);
    WriteInfoDeviceEvent(
        "device_kept",
        spec.name,
        JsonObject({
            {"instance_id", JsonText(devices[keepIndex].instanceId)},
            {"instance_count", JsonSize(devices.size())},
            {"duplicate_count", JsonSize(devices.size() - 1)}
        }));

    bool ok = true;
    for (size_t index = 0; index < devices.size(); ++index) {
        if (index == keepIndex) {
            continue;
        }

        bool needReboot = false;
        std::wstring errorMessage;
        if (!UninstallDeviceByInstanceId(devices[index].instanceId, &needReboot, &errorMessage)) {
            WriteErrorDeviceEvent(
                "device_remove_failed",
                spec.name,
                JsonObject({
                    {"instance_id", JsonText(devices[index].instanceId)},
                    {"error", JsonText(errorMessage)}
                }));
            ok = false;
            continue;
        }

        WriteInfoDeviceEvent(
            "device_removed",
            spec.name,
            JsonObject({
                {"instance_id", JsonText(devices[index].instanceId)},
                {"need_reboot", Json::Value(needReboot)}
            }));
    }

    return ok;
}

static bool RemoveAllDevices(const DeviceSpec& spec) {
    const std::vector<DeviceInstance> devices = EnumerateDevicesByHardwareId(spec);
    bool ok = true;

    for (const DeviceInstance& device : devices) {
        bool needReboot = false;
        std::wstring errorMessage;
        if (!UninstallDeviceByInstanceId(device.instanceId, &needReboot, &errorMessage)) {
            WriteErrorDeviceEvent(
                "device_remove_failed",
                spec.name,
                JsonObject({
                    {"instance_id", JsonText(device.instanceId)},
                    {"error", JsonText(errorMessage)}
                }));
            ok = false;
            continue;
        }

        WriteInfoDeviceEvent(
            "device_removed",
            spec.name,
            JsonObject({
                {"instance_id", JsonText(device.instanceId)},
                {"need_reboot", Json::Value(needReboot)}
            }));
    }

    if (devices.empty()) {
        WriteInfoDeviceEvent(
            "device_absent",
            spec.name,
            JsonObject({
                {"instance_count", JsonSize(0)}
            }));
    }

    return ok;
}

static bool UninstallDriverPackage(const DeviceSpec& spec, const fs::path& infPath) {
    HMODULE newdevModule = LoadLibraryW(L"newdev.dll");
    if (newdevModule == nullptr) {
        const DWORD error = GetLastError();
        WriteErrorDeviceEvent(
            "package_uninstall_failed",
            spec.name,
            JsonObject({
                {"source_inf", JsonText(infPath.native())},
                {"error_code", JsonUint(error)},
                {"error", JsonText(GetLastErrorMessage(error))}
            }));
        return false;
    }

    const auto uninstallDriver =
        reinterpret_cast<DiUninstallDriverWFn>(GetProcAddress(newdevModule, "DiUninstallDriverW"));
    if (uninstallDriver == nullptr) {
        const DWORD error = GetLastError();
        WriteErrorDeviceEvent(
            "package_uninstall_failed",
            spec.name,
            JsonObject({
                {"source_inf", JsonText(infPath.native())},
                {"error_code", JsonUint(error)},
                {"error", JsonText("DiUninstallDriverW not available in newdev.dll")}
            }));
        FreeLibrary(newdevModule);
        return false;
    }

    BOOL needReboot = FALSE;
    if (!uninstallDriver(nullptr, infPath.c_str(), 0, &needReboot)) {
        const DWORD error = GetLastError();
        WriteErrorDeviceEvent(
            "package_uninstall_failed",
            spec.name,
            JsonObject({
                {"source_inf", JsonText(infPath.native())},
                {"error_code", JsonUint(error)},
                {"error", JsonText(GetLastErrorMessage(error))}
            }));
        FreeLibrary(newdevModule);
        return false;
    }

    WriteInfoDeviceEvent(
        "package_uninstalled",
        spec.name,
        JsonObject({
            {"source_inf", JsonText(infPath.native())},
            {"need_reboot", Json::Value(needReboot != FALSE)}
        }));
    FreeLibrary(newdevModule);
    return true;
}

static bool RunInstall(const fs::path& packageRoot) {
    bool ok = true;
    for (const DeviceSpec& spec : kDeviceSpecs) {
        fs::path infPath;
        if (!TryFindSingleInfPath(spec, packageRoot, &infPath)) {
            ok = false;
            continue;
        }

        PackageState packageState{};
        if (!EnsureDriverPackage(spec, infPath, &packageState)) {
            ok = false;
            continue;
        }

        if (!EnsureUniqueDevice(spec)) {
            ok = false;
        }
    }

    return ok;
}

static bool RunUninstall(const fs::path& packageRoot) {
    bool ok = true;
    for (const DeviceSpec& spec : kDeviceSpecs) {
        if (!RemoveAllDevices(spec)) {
            ok = false;
        }

        fs::path infPath;
        if (!TryFindSingleInfPath(spec, packageRoot, &infPath)) {
            ok = false;
            continue;
        }

        if (!UninstallDriverPackage(spec, infPath)) {
            ok = false;
        }
    }

    return ok;
}

static void PrintUsage() {
    WriteInfoEvent(
        "usage",
        JsonObject({
            {"syntax", JsonText("devtidy [install|uninstall] [--package-root <path>]")}
        }));
}

int wmain(int argc, wchar_t** argv) {
    fs::path packageRoot;
    RunMode mode = RunMode::Install;
    bool modeSeen = false;

    for (int index = 1; index < argc; ++index) {
        const std::wstring arg = argv[index];
        if (arg == L"install") {
            if (modeSeen) {
                PrintUsage();
                return 1;
            }

            mode = RunMode::Install;
            modeSeen = true;
            continue;
        }
        if (arg == L"uninstall") {
            if (modeSeen) {
                PrintUsage();
                return 1;
            }

            mode = RunMode::Uninstall;
            modeSeen = true;
            continue;
        }
        if (arg == L"--package-root") {
            if (index + 1 >= argc) {
                PrintUsage();
                return 1;
            }

            packageRoot = fs::path(argv[++index]);
            continue;
        }
        if (arg == L"--help" || arg == L"-h") {
            PrintUsage();
            return 0;
        }

        PrintUsage();
        return 1;
    }

    if (packageRoot.empty()) {
        packageRoot = GetPackageRootFromExecutable();
    }

    if (packageRoot.empty()) {
        WriteErrorEvent(
            "package_root_not_found",
            JsonObject({
                {"error", JsonText("unable to resolve package root from executable path")},
                {"hint", JsonText("pass --package-root <path>")}
            }));
        return 1;
    }

    WriteInfoEvent(
        "package_root",
        JsonObject({
            {"package_root", JsonText(packageRoot.native())}
        }));

    const bool ok =
        (mode == RunMode::Install) ? RunInstall(packageRoot) : RunUninstall(packageRoot);

    WriteInfoEvent(
        "summary",
        JsonObject({
            {"ok", Json::Value(ok)}
        }));
    return ok ? 0 : 1;
}
