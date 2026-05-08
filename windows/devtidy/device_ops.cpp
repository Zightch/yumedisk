#include "device_ops.h"

#include <cfgmgr32.h>
#include <newdev.h>
#include <setupapi.h>

#include <vector>

namespace devtidy {

namespace {

using DiUninstallDeviceFn = BOOL(WINAPI*)(HWND, HDEVINFO, PSP_DEVINFO_DATA, DWORD, PBOOL);
using DiUninstallDriverWFn = BOOL(WINAPI*)(HWND, LPCWSTR, DWORD, PBOOL);

std::vector<DeviceInstance> EnumerateDevicesByHardwareId(const DeviceSpec& spec) {
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

bool UninstallDeviceByInstanceId(
    const std::wstring& instanceId,
    bool* needReboot,
    std::wstring* errorMessage
) {
    HMODULE newdevModule = LoadLibraryW(L"newdev.dll");
    if (newdevModule == nullptr) {
        if (errorMessage != nullptr) {
            *errorMessage = GetLastErrorMessage(GetLastError());
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
            *errorMessage = GetLastErrorMessage(GetLastError());
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
                *errorMessage = GetLastErrorMessage(GetLastError());
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

int DeviceRank(const DeviceInstance& instance) {
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

size_t ChoosePrimaryDeviceIndex(const std::vector<DeviceInstance>& devices) {
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

bool CreateRootDevice(const DeviceSpec& spec, std::wstring* createdInstanceId) {
    HDEVINFO info = SetupDiCreateDeviceInfoList(spec.classGuid, nullptr);
    if (info == INVALID_HANDLE_VALUE) {
        WriteErrorDeviceEvent(
            "device_info_list_failed",
            spec.name,
            JsonObject({
                {"error_code", JsonUint(GetLastError())},
                {"error", JsonText(GetLastErrorMessage(GetLastError()))}
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

bool BindDriverToHardwareId(const DeviceSpec& spec, const fs::path& infPath) {
    BOOL rebootRequired = FALSE;

    if (!UpdateDriverForPlugAndPlayDevicesW(
            nullptr,
            spec.hardwareId,
            infPath.c_str(),
            INSTALLFLAG_FORCE,
            &rebootRequired)) {
        const DWORD error = GetLastError();
        WriteErrorDeviceEvent(
            "device_bind_failed",
            spec.name,
            JsonObject({
                {"source_inf", JsonText(infPath.native())},
                {"hardware_id", JsonText(spec.hardwareId)},
                {"error_code", JsonUint(error)},
                {"error", JsonText(GetLastErrorMessage(error))}
            }));
        return false;
    }

    WriteInfoDeviceEvent(
        "device_bound",
        spec.name,
        JsonObject({
            {"source_inf", JsonText(infPath.native())},
            {"hardware_id", JsonText(spec.hardwareId)},
            {"need_reboot", Json::Value(rebootRequired != FALSE)}
        }));
    return true;
}

}  // namespace

bool EnsureDriverPackage(const DeviceSpec& spec, const fs::path& infPath) {
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

        WriteInfoDeviceEvent(
            "package_present",
            spec.name,
            JsonObject({
                {"source_inf", JsonText(infPath.native())}
            }));
        return true;
    }

    WriteInfoDeviceEvent(
        "package_staged",
        spec.name,
        JsonObject({
            {"published_inf", JsonText(publishedInf)}
        }));
    return true;
}

bool EnsureUniqueDevice(const DeviceSpec& spec, const fs::path& infPath) {
    std::vector<DeviceInstance> devices = EnumerateDevicesByHardwareId(spec);

    if (devices.empty()) {
        std::wstring createdInstanceId;
        if (!CreateRootDevice(spec, &createdInstanceId)) {
            return false;
        }

        if (!BindDriverToHardwareId(spec, infPath)) {
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

    if (!BindDriverToHardwareId(spec, infPath)) {
        return false;
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

bool RemoveAllDevices(const DeviceSpec& spec) {
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

bool UninstallDriverPackage(const DeviceSpec& spec, const fs::path& infPath) {
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

}  // namespace devtidy
