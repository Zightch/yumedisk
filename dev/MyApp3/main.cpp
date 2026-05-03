#include <Windows.h>
#include <iostream>
#include <regex>

#define MSG_CODE_READ CTL_CODE(FILE_DEVICE_UNKNOWN, 0x800, METHOD_OUT_DIRECT, FILE_READ_ACCESS)
#define MSG_CODE_WRITE CTL_CODE(FILE_DEVICE_UNKNOWN, 0x801, METHOD_IN_DIRECT, FILE_WRITE_ACCESS)
std::regex readCmd(R"(^read (\d+)$)");
std::regex writeCmd(R"(^write (.*)$)");
std::regex ioCtlReadCmd(R"(^IOCTL read (\d+)$)");
std::regex ioCtlWriteCmd(R"(^IOCTL write (.*)$)");

int main(int argc, char* argv[]) {
    if (argc != 2) {
        std::cout << "usage: " << argv[0] << " <file path>" << std::endl;
        return 0;
    }

    HANDLE file = CreateFileA(
        argv[1],
        GENERIC_READ | GENERIC_WRITE,
        0,
        NULL,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        NULL
    );
    DWORD err = GetLastError();
    if (err != 0) {
        std::cout << "open fail, err code: " << err << ", file: " << file << std::endl;
        return -1;
    }
    
    std::cout << "open ok" << std::endl;
    while (true) {
        std::cout << "\n>";
        std::string cmd;
        std::getline(std::cin, cmd);
        if (cmd == "exit")
            break;
        std::smatch matches;
        if (std::regex_search(cmd, matches, readCmd)) {
            int size = std::stoi(matches[1]);
            char* buffer = new char[size];
            memset(buffer, 0, size);
            DWORD bytesRead;
            BOOL result = ReadFile(file, buffer, size, &bytesRead, NULL);
            if (result) {
                std::cout << "read ok, bytes read: " << bytesRead << ", data: " << buffer << std::endl;
            } else {
                err = GetLastError();
                std::cout << "read fail, err code: " << err << std::endl;
            }
            delete[] buffer;
        }
        if (std::regex_search(cmd, matches, writeCmd)) {
            std::string data = matches[1];
            DWORD bytesWritten;
            BOOL result = WriteFile(file, data.c_str(), data.size(), &bytesWritten, NULL);
            if (result) {
                std::cout << "write ok, bytes written: " << bytesWritten << std::endl;
            } else {
                err = GetLastError();
                std::cout << "write fail, err code: " << err << std::endl;
            }
        }
        if (std::regex_search(cmd, matches, ioCtlReadCmd)) {
            int size = std::stoi(matches[1]);
            char* buffer = new char[size];
            memset(buffer, 0, size);
            DWORD bytesReturned;
            BOOL result = DeviceIoControl(file, MSG_CODE_READ, NULL, 0, buffer, size, &bytesReturned, NULL);
            if (result) {
                std::cout << "read ok, bytes returned: " << bytesReturned << ", data: " << buffer << std::endl;
            } else {
                err = GetLastError();
                std::cout << "read fail, err code: " << err << std::endl;
            }
            delete[] buffer;
        }
        if (std::regex_search(cmd, matches, ioCtlWriteCmd)) {
            std::string data = matches[1];
            DWORD bytesReturned;
            BOOL result = DeviceIoControl(file, MSG_CODE_WRITE, (LPVOID)data.c_str(), data.size(), NULL, 0, &bytesReturned, NULL);
            if (result) {
                std::cout << "write ok, bytes returned: " << bytesReturned << std::endl;
            } else {
                err = GetLastError();
                std::cout << "write fail, err code: " << err << std::endl;
            }
        }
    }
    CloseHandle(file);
    file = NULL;
        
    return 0;
}
