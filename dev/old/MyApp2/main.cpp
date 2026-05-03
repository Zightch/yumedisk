#include <Windows.h>
#include <iostream>

#define MSG_CODE_READ CTL_CODE(FILE_DEVICE_UNKNOWN, 0x800, METHOD_IN_DIRECT, FILE_READ_ACCESS)
#define MSG_CODE_WRITE CTL_CODE(FILE_DEVICE_UNKNOWN, 0x801, METHOD_OUT_DIRECT, FILE_WRITE_ACCESS)

int main() {
    HANDLE file = CreateFileW(L"\\\\.\\MyDriver2", GENERIC_ALL, 0, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    DWORD err = GetLastError();
    if (err == 0) {
        std::cout << "open ok" << std::endl;

        char buffer[512] = {0};
        std::cout << "buffer addr: " << (void*)buffer << std::endl;
        DWORD size = 0;
        ReadFile(file, buffer, sizeof(buffer), &size, NULL);
        std::cout << "read from driver: " << buffer << std::endl;

        memset(buffer, 0, sizeof(buffer));
        strcpy(buffer, "hello from usermode app2");
        DeviceIoControl(file, MSG_CODE_WRITE, buffer, sizeof(buffer), NULL, 0, &size, NULL);
        std::cout << "return size: " << size << std::endl;

        memset(buffer, 0, sizeof(buffer));
        DeviceIoControl(file, MSG_CODE_READ, NULL, 0, buffer, sizeof(buffer), &size, NULL);
        std::cout << "return size: " << size << ", data: " << buffer << std::endl;
        
        CloseHandle(file);
        file = NULL;
    } else
        std::cout << "open fail, err code: " << err << std::endl;
    return 0;
}
