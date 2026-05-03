#include <Windows.h>
#include <iostream>

#define DC_READ CTL_CODE(FILE_DEVICE_UNKNOWN, 0x800, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define DC_WRITE CTL_CODE(FILE_DEVICE_UNKNOWN, 0x801, METHOD_BUFFERED, FILE_ANY_ACCESS)

int main() {
    HANDLE file = CreateFileW(L"\\\\.\\MyDriver1", GENERIC_ALL, 0, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    DWORD err = GetLastError();
    if (err == 0) {
        std::cout << "open ok" << std::endl;
        std::string data = "Hello, driver!";
        WriteFile(file, data.c_str(), data.size() + 1, NULL, NULL);
        char rBuffer[512] = {0};
        DWORD size = 0;
        ReadFile(file, rBuffer, sizeof(rBuffer), &size, NULL);
        std::cout << "read from driver: " << rBuffer << std::endl;
        
        char wBuffer[512] = {0};
        DWORD rSize = 0;
        // DeviceIoControl(file, DC_READ, wBuffer, 512, NULL, 0, &rSize, NULL);
        // DeviceIoControl(file, DC_WRITE, NULL, 0, rBuffer, 512, &rSize, NULL);
        
        CloseHandle(file);
        file = NULL;
    } else
        std::cout << "open fail, err code: " << err << std::endl;
    return 0;
}
