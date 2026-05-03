#include "utils.h"

#include <SetupAPI.h>
#include <cfgmgr32.h>

#include <cstdlib>
#include <cstring>
#include <iostream>
#include <vector>

DWORD RmDevWithID(const std::wstring& devID) {
    HDEVINFO hDevInfo;

    hDevInfo = SetupDiGetClassDevsW(nullptr, nullptr, nullptr, DIGCF_ALLCLASSES);
    if (hDevInfo == INVALID_HANDLE_VALUE) {
        return GetLastError();
    }

    DWORD ret = 0;
    for (DWORD i = 0;; ++i) {
        SP_DEVINFO_DATA devInfoData{};
        devInfoData.cbSize = sizeof(devInfoData);
        if (!SetupDiEnumDeviceInfo(hDevInfo, i, &devInfoData)) {
            if (GetLastError() == ERROR_NO_MORE_ITEMS) {
                break;
            }
            continue;
        }

        WCHAR getDevID[MAX_PATH] = {};
        if (CM_Get_Device_IDW(devInfoData.DevInst, getDevID, ARRAYSIZE(getDevID), 0) != CR_SUCCESS) {
            continue;
        }

        if (devID != getDevID) {
            continue;
        }

        ret = SetupDiRemoveDevice(hDevInfo, &devInfoData) ? 0 : GetLastError();
        break;
    }

    SetupDiDestroyDeviceInfoList(hDevInfo);
    return ret;
}

std::vector<DevInfo> EnumDev() {
    HDEVINFO info;
    std::vector<DevInfo> devInfos;

    info = SetupDiGetClassDevsW(nullptr, nullptr, nullptr, DIGCF_ALLCLASSES);
    if (info == INVALID_HANDLE_VALUE) {
        return {};
    }

    for (DWORD i = 0;; ++i) {
        SP_DEVINFO_DATA devInfoData{};
        WCHAR devID[MAX_PATH] = {};
        WCHAR description[256] = {};
        WCHAR friendlyName[256] = {};
        WCHAR driverName[256] = {};
        DWORD status = 0;
        DWORD problem = 0;
        DevInfo devInfo;

        devInfoData.cbSize = sizeof(devInfoData);
        if (!SetupDiEnumDeviceInfo(info, i, &devInfoData)) {
            if (GetLastError() == ERROR_NO_MORE_ITEMS) {
                break;
            }
            continue;
        }

        if (CM_Get_Device_IDW(devInfoData.DevInst, devID, ARRAYSIZE(devID), 0) != CR_SUCCESS) {
            continue;
        }

        SetupDiGetDeviceRegistryPropertyW(
            info,
            &devInfoData,
            SPDRP_DRIVER,
            nullptr,
            reinterpret_cast<PBYTE>(description),
            sizeof(description),
            nullptr
        );

        SetupDiGetDeviceRegistryPropertyW(
            info,
            &devInfoData,
            SPDRP_FRIENDLYNAME,
            nullptr,
            reinterpret_cast<PBYTE>(friendlyName),
            sizeof(friendlyName),
            nullptr
        );

        HKEY devKey = SetupDiOpenDevRegKey(
            info,
            &devInfoData,
            DICS_FLAG_GLOBAL,
            0,
            DIREG_DRV,
            KEY_READ
        );
        if (devKey != INVALID_HANDLE_VALUE) {
            DWORD size = sizeof(driverName);
            RegQueryValueExW(devKey, L"DriverDesc", nullptr, nullptr, reinterpret_cast<PBYTE>(driverName), &size);
            RegCloseKey(devKey);
        }

        devInfo.driverName = driverName;
        devInfo.friendlyName = friendlyName;
        devInfo.devID = devID;
        devInfo.desc = description;

        if (CM_Get_DevNode_Status(&status, &problem, devInfoData.DevInst, 0) == CR_SUCCESS) {
            devInfo.statusEnable = true;
            devInfo.status = status;
            devInfo.problem = problem;
        }

        devInfos.push_back(devInfo);
    }

    SetupDiDestroyDeviceInfoList(info);
    return devInfos;
}

std::vector<std::pair<DWORD, std::wstring>> GetDevPathWithGUID(const GUID* guid) {
    std::vector<std::pair<DWORD, std::wstring>> devPaths;
    HDEVINFO info = SetupDiGetClassDevsW(guid, nullptr, nullptr, DIGCF_DEVICEINTERFACE | DIGCF_PRESENT);
    if (info == INVALID_HANDLE_VALUE) {
        return {};
    }

    for (DWORD i = 0;; ++i) {
        SP_DEVICE_INTERFACE_DATA ifdata{};
        ifdata.cbSize = sizeof(ifdata);
        if (!SetupDiEnumDeviceInterfaces(info, nullptr, guid, i, &ifdata)) {
            if (GetLastError() == ERROR_NO_MORE_ITEMS) {
                break;
            }
            devPaths.emplace_back(GetLastError(), L"");
            continue;
        }

        DWORD detailSize = 0;
        SetupDiGetDeviceInterfaceDetailW(info, &ifdata, nullptr, 0, &detailSize, nullptr);
        if (detailSize == 0) {
            devPaths.emplace_back(GetLastError(), L"");
            continue;
        }

        auto detail = reinterpret_cast<PSP_DEVICE_INTERFACE_DETAIL_DATA_W>(malloc(detailSize));
        if (detail == nullptr) {
            devPaths.emplace_back(ERROR_OUTOFMEMORY, L"");
            continue;
        }

        memset(detail, 0, detailSize);
        detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
        if (!SetupDiGetDeviceInterfaceDetailW(info, &ifdata, detail, detailSize, nullptr, nullptr)) {
            DWORD error = GetLastError();
            free(detail);
            devPaths.emplace_back(error, L"");
            continue;
        }

        devPaths.emplace_back(0, detail->DevicePath);
        free(detail);
    }

    SetupDiDestroyDeviceInfoList(info);
    return devPaths;
}

std::string hex(const char* buffer, unsigned int size) {
    std::string result;
    for (unsigned int i = 0; i < size; ++i) {
        int low = buffer[i] & 0x0F;
        int high = (buffer[i] >> 4) & 0x0F;

        result += (high <= 9) ? static_cast<char>('0' + high) : static_cast<char>('A' + high - 10);
        result += (low <= 9) ? static_cast<char>('0' + low) : static_cast<char>('A' + low - 10);
        result += ' ';
    }

    if (!result.empty()) {
        result.pop_back();
    }

    return result;
}

HANDLE OpenFirstDeviceInterface(const GUID* guid, bool overlapped) {
    auto devPaths = GetDevPathWithGUID(guid);
    for (const auto& item : devPaths) {
        if (item.first != 0) {
            continue;
        }

        HANDLE file = CreateFileW(
            item.second.c_str(),
            GENERIC_READ | GENERIC_WRITE,
            0,
            nullptr,
            OPEN_EXISTING,
            overlapped ? FILE_FLAG_OVERLAPPED : FILE_ATTRIBUTE_NORMAL,
            nullptr
        );

        if (file != INVALID_HANDLE_VALUE) {
            return file;
        }
    }

    return INVALID_HANDLE_VALUE;
}

bool IoctlBuffered(HANDLE file, DWORD code, void* buffer, DWORD inSize, DWORD outSize, DWORD* bytesReturned, OVERLAPPED* overlapped) {
    DWORD localBytesReturned = 0;
    BOOL ok = DeviceIoControl(
        file,
        code,
        buffer,
        inSize,
        buffer,
        outSize,
        bytesReturned != nullptr ? bytesReturned : &localBytesReturned,
        overlapped
    );

    if (!ok && overlapped != nullptr && GetLastError() == ERROR_IO_PENDING) {
        return true;
    }

    return ok == TRUE;
}
