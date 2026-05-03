#define NOMINMAX

#include <Windows.h>
#include <cfgmgr32.h>
#include <devguid.h>
#include <setupapi.h>

#include <algorithm>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

struct DeviceSpec {
    const wchar_t* name;
    const wchar_t* hardwareId;
    const wchar_t* className;
    const GUID* classGuid;
    const wchar_t* infRelativePath;
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
        L"windows\\YumeDiskSCSI\\YumeDiskSCSI\\YumeDiskSCSI.inf"
    },
    {
        L"YumeDiskKMDF",
        L"Root\\YumeDiskKMDF",
        L"System",
        &GUID_DEVCLASS_SYSTEM,
        L"windows\\YumeDiskKMDF\\YumeDiskKMDF\\YumeDiskKMDF.inf"
    }
};

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

static bool EndsWithInsensitive(const std::wstring& value, const std::wstring& suffix) {
    if (value.size() < suffix.size()) {
        return false;
    }

    const size_t offset = value.size() - suffix.size();
    for (size_t i = 0; i < suffix.size(); ++i) {
        if (towlower(value[offset + i]) != towlower(suffix[i])) {
            return false;
        }
    }

    return true;
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

static bool LooksLikeRepoRoot(const fs::path& candidate) {
    return fs::exists(candidate / L"windows\\YumeDiskSCSI\\YumeDiskSCSI\\YumeDiskSCSI.inf") &&
        fs::exists(candidate / L"windows\\YumeDiskKMDF\\YumeDiskKMDF\\YumeDiskKMDF.inf");
}

static fs::path FindRepoRoot() {
    std::vector<fs::path> seeds;
    std::error_code ec;
    fs::path current = fs::current_path(ec);
    if (!ec) {
        seeds.push_back(current);
    }

    fs::path exePath = GetExecutablePath();
    if (!exePath.empty()) {
        seeds.push_back(exePath.parent_path());
    }

    for (const fs::path& seed : seeds) {
        fs::path cursor = seed;
        while (!cursor.empty()) {
            if (LooksLikeRepoRoot(cursor)) {
                return cursor;
            }

            fs::path parent = cursor.parent_path();
            if (parent == cursor) {
                break;
            }
            cursor = parent;
        }
    }

    return {};
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
            *errorMessage = GetLastErrorMessage(GetLastError());
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
                *errorMessage = GetLastErrorMessage(GetLastError());
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
            std::wcerr << L"[" << spec.name << L"] stage driver package failed: "
                       << GetLastErrorMessage(error) << std::endl;
            return false;
        }

        if (state != nullptr) {
            state->alreadyPresent = true;
            state->publishedInf.clear();
        }

        std::wcout << L"[" << spec.name << L"] driver package already present" << std::endl;
        return true;
    }

    if (state != nullptr) {
        state->alreadyPresent = false;
        state->publishedInf = publishedInf;
    }

    std::wcout << L"[" << spec.name << L"] staged driver package: " << publishedInf << std::endl;
    return true;
}

static bool CreateRootDevice(const DeviceSpec& spec, std::wstring* createdInstanceId) {
    HDEVINFO info = SetupDiCreateDeviceInfoList(spec.classGuid, nullptr);
    if (info == INVALID_HANDLE_VALUE) {
        std::wcerr << L"[" << spec.name << L"] create device info list failed: "
                   << GetLastErrorMessage(GetLastError()) << std::endl;
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
        std::wcerr << L"[" << spec.name << L"] create device info failed: "
                   << GetLastErrorMessage(GetLastError()) << std::endl;
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
        std::wcerr << L"[" << spec.name << L"] set hardware id failed: "
                   << GetLastErrorMessage(GetLastError()) << std::endl;
        SetupDiDestroyDeviceInfoList(info);
        return false;
    }

    if (!SetupDiCallClassInstaller(DIF_REGISTERDEVICE, info, &devInfoData)) {
        std::wcerr << L"[" << spec.name << L"] register device failed: "
                   << GetLastErrorMessage(GetLastError()) << std::endl;
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

        std::wcout << L"[" << spec.name << L"] created device";
        if (!createdInstanceId.empty()) {
            std::wcout << L": " << createdInstanceId;
        }
        std::wcout << std::endl;
        return true;
    }

    const size_t keepIndex = ChoosePrimaryDeviceIndex(devices);
    std::wcout << L"[" << spec.name << L"] keeping device: " << devices[keepIndex].instanceId << std::endl;

    bool ok = true;
    for (size_t index = 0; index < devices.size(); ++index) {
        if (index == keepIndex) {
            continue;
        }

        std::wstring errorMessage;
        if (!RemoveDeviceByInstanceId(devices[index].instanceId, &errorMessage)) {
            std::wcerr << L"[" << spec.name << L"] remove duplicate failed: "
                       << devices[index].instanceId << L": " << errorMessage << std::endl;
            ok = false;
            continue;
        }

        std::wcout << L"[" << spec.name << L"] removed duplicate: "
                   << devices[index].instanceId << std::endl;
    }

    return ok;
}

static void PrintUsage() {
    std::wcout << L"usage: devtidy [--repo-root <path>]" << std::endl;
}

int wmain(int argc, wchar_t** argv) {
    fs::path repoRoot;

    for (int index = 1; index < argc; ++index) {
        const std::wstring arg = argv[index];
        if (arg == L"--repo-root") {
            if (index + 1 >= argc) {
                PrintUsage();
                return 1;
            }

            repoRoot = fs::path(argv[++index]);
            continue;
        }
        if (arg == L"--help" || arg == L"-h") {
            PrintUsage();
            return 0;
        }

        PrintUsage();
        return 1;
    }

    if (repoRoot.empty()) {
        repoRoot = FindRepoRoot();
    }

    if (repoRoot.empty()) {
        std::wcerr << L"repo root not found; pass --repo-root <path>" << std::endl;
        return 1;
    }

    std::wcout << L"repo root: " << repoRoot.native() << std::endl;

    bool ok = true;
    for (const DeviceSpec& spec : kDeviceSpecs) {
        const fs::path infPath = repoRoot / spec.infRelativePath;
        if (!fs::exists(infPath)) {
            std::wcerr << L"[" << spec.name << L"] inf not found: " << infPath.native() << std::endl;
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

    return ok ? 0 : 1;
}
