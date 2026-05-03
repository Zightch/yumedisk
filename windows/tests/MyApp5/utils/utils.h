#pragma once

#include <Windows.h>

#include <string>
#include <vector>

typedef struct DEV_INFO {
    std::wstring desc;
    std::wstring devID;
    std::wstring friendlyName;
    std::wstring driverName;
    bool statusEnable{};
    DWORD status{};
    DWORD problem{};
} DevInfo;

DWORD RmDevWithID(const std::wstring& devID);
std::vector<DevInfo> EnumDev();
std::vector<std::pair<DWORD, std::wstring>> GetDevPathWithGUID(const GUID* guid);
std::string hex(const char* buffer, unsigned int size);

HANDLE OpenFirstDeviceInterface(const GUID* guid, bool overlapped = false);
bool IoctlBuffered(HANDLE file, DWORD code, void* buffer, DWORD inSize, DWORD outSize, DWORD* bytesReturned, OVERLAPPED* overlapped = nullptr);
