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

static void WriteJsonEvent(
    const char* level,
    const char* event,
    std::initializer_list<std::pair<const char*, Json::Value>> fields
) {
    Json::Value root(Json::objectValue);
    root["level"] = level;
    root["event"] = event;
    for (const auto& field : fields) {
        root[field.first] = field.second;
    }

    Json::StreamWriterBuilder builder;
    builder["indentation"] = "";
    builder["commentStyle"] = "None";
    builder["enableYAMLCompatibility"] = false;

    std::cout << Json::writeString(builder, root) << std::endl;
}

static void WriteInfoEvent(
    const char* event,
    std::initializer_list<std::pair<const char*, Json::Value>> fields = {}
) {
    WriteJsonEvent("info", event, fields);
}

static void WriteErrorEvent(
    const char* event,
    std::initializer_list<std::pair<const char*, Json::Value>> fields = {}
) {
    WriteJsonEvent("error", event, fields);
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

static bool RemoveDeviceByInstanceId(const std::wstring& instanceId, std::wstring* errorMessage) {
    HDEVINFO info = SetupDiGetClassDevsW(nullptr, nullptr, nullptr, DIGCF_ALLCLASSES);
    if (info == INVALID_HANDLE_VALUE) {
        if (errorMessage != nullptr) {
            const DWORD error = GetLastError();
            *errorMessage = GetLastErrorMessage(error);
        }
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

        if (!SetupDiRemoveDevice(info, &devInfoData)) {
            if (errorMessage != nullptr) {
                const DWORD error = GetLastError();
                *errorMessage = GetLastErrorMessage(error);
            }
            SetupDiDestroyDeviceInfoList(info);
            return false;
        }

        removed = true;
        break;
    }

    SetupDiDestroyDeviceInfoList(info);
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
        WriteErrorEvent(
            "package_dir_not_found",
            {
                {"device", JsonText(spec.name)},
                {"path", JsonText(packageDir.native())}
            });
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
        WriteErrorEvent(
            "package_dir_scan_failed",
            {
                {"device", JsonText(spec.name)},
                {"path", JsonText(packageDir.native())},
                {"error", JsonText(ec.message())}
            });
        return false;
    }

    std::sort(infFiles.begin(), infFiles.end());
    if (infFiles.empty()) {
        WriteErrorEvent(
            "inf_not_found",
            {
                {"device", JsonText(spec.name)},
                {"path", JsonText(packageDir.native())}
            });
        return false;
    }
    if (infFiles.size() > 1) {
        Json::Value found(Json::arrayValue);
        for (const fs::path& candidate : infFiles) {
            found.append(WideToUtf8(candidate.filename().native()));
        }

        WriteErrorEvent(
            "multiple_inf_found",
            {
                {"device", JsonText(spec.name)},
                {"path", JsonText(packageDir.native())},
                {"files", found}
            });
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
            WriteErrorEvent(
                "package_stage_failed",
                {
                    {"device", JsonText(spec.name)},
                    {"inf", JsonText(infPath.native())},
                    {"error_code", JsonUint(error)},
                    {"error", JsonText(GetLastErrorMessage(error))}
                });
            return false;
        }

        if (state != nullptr) {
            state->alreadyPresent = true;
            state->publishedInf.clear();
        }

        WriteInfoEvent(
            "package_present",
            {
                {"device", JsonText(spec.name)},
                {"inf", JsonText(infPath.native())}
            });
        return true;
    }

    if (state != nullptr) {
        state->alreadyPresent = false;
        state->publishedInf = publishedInf;
    }

    WriteInfoEvent(
        "package_staged",
        {
            {"device", JsonText(spec.name)},
            {"inf", JsonText(publishedInf)}
        });
    return true;
}

static bool CreateRootDevice(const DeviceSpec& spec, std::wstring* createdInstanceId) {
    HDEVINFO info = SetupDiCreateDeviceInfoList(spec.classGuid, nullptr);
    if (info == INVALID_HANDLE_VALUE) {
        const DWORD error = GetLastError();
        WriteErrorEvent(
            "device_info_list_failed",
            {
                {"device", JsonText(spec.name)},
                {"error_code", JsonUint(error)},
                {"error", JsonText(GetLastErrorMessage(error))}
            });
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
        WriteErrorEvent(
            "device_create_failed",
            {
                {"device", JsonText(spec.name)},
                {"error_code", JsonUint(error)},
                {"error", JsonText(GetLastErrorMessage(error))}
            });
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
        WriteErrorEvent(
            "device_hwid_failed",
            {
                {"device", JsonText(spec.name)},
                {"error_code", JsonUint(error)},
                {"error", JsonText(GetLastErrorMessage(error))}
            });
        SetupDiDestroyDeviceInfoList(info);
        return false;
    }

    if (!SetupDiCallClassInstaller(DIF_REGISTERDEVICE, info, &devInfoData)) {
        const DWORD error = GetLastError();
        WriteErrorEvent(
            "device_register_failed",
            {
                {"device", JsonText(spec.name)},
                {"error_code", JsonUint(error)},
                {"error", JsonText(GetLastErrorMessage(error))}
            });
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

        WriteInfoEvent(
            "device_created",
            {
                {"device", JsonText(spec.name)},
                {"instance_id", JsonText(createdInstanceId)}
            });
        return true;
    }

    const size_t keepIndex = ChoosePrimaryDeviceIndex(devices);
    WriteInfoEvent(
        "device_kept",
        {
            {"device", JsonText(spec.name)},
            {"instance_id", JsonText(devices[keepIndex].instanceId)}
        });

    bool ok = true;
    for (size_t index = 0; index < devices.size(); ++index) {
        if (index == keepIndex) {
            continue;
        }

        std::wstring errorMessage;
        if (!RemoveDeviceByInstanceId(devices[index].instanceId, &errorMessage)) {
            WriteErrorEvent(
                "device_remove_failed",
                {
                    {"device", JsonText(spec.name)},
                    {"instance_id", JsonText(devices[index].instanceId)},
                    {"error", JsonText(errorMessage)}
                });
            ok = false;
            continue;
        }

        WriteInfoEvent(
            "device_removed",
            {
                {"device", JsonText(spec.name)},
                {"instance_id", JsonText(devices[index].instanceId)}
            });
    }

    return ok;
}

static void PrintUsage() {
    WriteInfoEvent(
        "usage",
        {
            {"syntax", JsonText("devtidy [--package-root <path>]")}
        });
}

int wmain(int argc, wchar_t** argv) {
    fs::path packageRoot;

    for (int index = 1; index < argc; ++index) {
        const std::wstring arg = argv[index];
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
            {
                {"error", JsonText("pass --package-root <path>")}
            });
        return 1;
    }

    WriteInfoEvent(
        "package_root",
        {
            {"path", JsonText(packageRoot.native())}
        });

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

    WriteInfoEvent(
        "summary",
        {
            {"ok", Json::Value(ok)}
        });
    return ok ? 0 : 1;
}
