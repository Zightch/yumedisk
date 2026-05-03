#include <Windows.h>
#include <iostream>
#include <regex>

const char* helpInfo = R"(available commands:
    read <size>
    write <data>
    IOCTL read <size>
    IOCTL write <data>
    cancel <io request id>
    stat <io request id>
    ls
    clear
    exit
)";

#define MSG_CODE_READ CTL_CODE(FILE_DEVICE_UNKNOWN, 0x800, METHOD_OUT_DIRECT, FILE_READ_ACCESS)
#define MSG_CODE_WRITE CTL_CODE(FILE_DEVICE_UNKNOWN, 0x801, METHOD_IN_DIRECT, FILE_WRITE_ACCESS)

enum IoResponseType {
    IO_RESPONSE_READ,
    IO_RESPONSE_WRITE,
    IO_RESPONSE_IOCTL_READ,
    IO_RESPONSE_IOCTL_WRITE
};

std::regex readCmd(R"(^read (\d+)$)");
std::regex writeCmd(R"(^write (.*)$)");
std::regex ioCtlReadCmd(R"(^IOCTL read (\d+)$)");
std::regex ioCtlWriteCmd(R"(^IOCTL write (.*)$)");
std::regex cancelCmd(R"(^cancel (\d+)$)");
std::regex statCmd(R"(^stat (\d+)$)");
struct IoRequest {
    OVERLAPPED overlapped;
    char* buffer;
    DWORD size;
    IoResponseType type;
};
std::map<UINT64, IoRequest> ioRequest;
UINT64 ioRequestId = 0;

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
        FILE_FLAG_OVERLAPPED,
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
        if (cmd == "exit") {
            bool existFailCancel = false;
            std::vector<UINT64> toCancel;
            for (auto& req : ioRequest) {
                bool result = CancelIoEx(file, &(req.second.overlapped));
                err = GetLastError();
                if (result) {
                    delete[] req.second.buffer;
                    toCancel.push_back(req.first);
                } else {
                    std::cout << "id: " << req.first << " cancel io request fail, err code: " << err << std::endl;
                    existFailCancel = true;
                }
            }
            for (const auto& id : toCancel)
                ioRequest.erase(id);
            if (existFailCancel)
                continue;
            break;
        }
        if (cmd == "help") {
            std::cout << helpInfo<< std::endl;
            continue;
        }
        if (cmd == "clear") {
            system("cls");
            continue;
        }
        if (cmd == "ls") {
            if (ioRequest.size() == 0)
                std::cout << "no io request" << std::endl;
            else {
                for (const auto& req : ioRequest) {
                    std::cout << "id: " << req.first << ", type: ";
                    switch (req.second.type) {
                        case IO_RESPONSE_READ:
                            std::cout << "read";
                            break;
                        case IO_RESPONSE_WRITE:
                            std::cout << "write";
                            break;
                        case IO_RESPONSE_IOCTL_READ:
                            std::cout << "IOCTL read";
                            break;
                        case IO_RESPONSE_IOCTL_WRITE:
                            std::cout << "IOCTL write";
                            break;
                    }
                    std::cout << std::endl << "    size: " << req.second.size << std::endl;
                    if (req.second.type == IO_RESPONSE_WRITE || req.second.type == IO_RESPONSE_IOCTL_WRITE)
                        std::cout << "    data: " << req.second.buffer << std::endl;
                }
            }
            continue;
        }
        std::smatch matches;
        if (std::regex_search(cmd, matches, readCmd)) {
            unsigned long long size = std::stoull(matches[1]);
            if (size > 0x00000000FFFFFFFF) {
                std::cout << "size too large" << std::endl;
                continue;
            }
            if (size == 0) {
                std::cout << "size cannot be zero" << std::endl;
                continue;
            }
            char* buffer = new char[size];
            DWORD retSize;
            memset(buffer, 0, size);
            ioRequest[ioRequestId] = { {}, buffer, (DWORD)size, IO_RESPONSE_READ };
            ReadFile(file, buffer, size, &retSize, &(ioRequest[ioRequestId].overlapped));
            err = GetLastError();
            if (err == ERROR_IO_PENDING) {
                std::cout << "read pending, io request id: " << ioRequestId <<  std::endl;
                ioRequestId++;
            } else {
                if (err != 0)
                    std::cout << "read fail, err code: " << err << std::endl;
                else
                    std::cout << "read completed immediately, size: " << retSize << ", data: " << std::string(buffer, retSize) << std::endl;
                ioRequest.erase(ioRequestId);
                delete[] buffer;
            }
        }
        if (std::regex_search(cmd, matches, writeCmd)) {
            std::string data = matches[1];
            DWORD retSize;
            char* buffer = new char[data.size()];
            memcpy(buffer, data.c_str(), data.size());
            ioRequest[ioRequestId] = { {}, buffer, (DWORD)data.size(), IO_RESPONSE_WRITE };
            WriteFile(file, buffer, data.size(), &retSize, &(ioRequest[ioRequestId].overlapped));
            err = GetLastError();
            if (err == ERROR_IO_PENDING) {
                std::cout << "write pending, io request id: " << ioRequestId <<  std::endl;
                ioRequestId++;
            } else {
                if (err != 0)
                    std::cout << "write fail, err code: " << err << std::endl;
                else
                    std::cout << "write completed immediately, bytes written: " << retSize << std::endl;
                ioRequest.erase(ioRequestId);
                delete[] buffer;
            }
        }
        if (std::regex_search(cmd, matches, ioCtlReadCmd)) {
            unsigned long long size = std::stoull(matches[1]);
            if (size > 0x00000000FFFFFFFF) {
                std::cout << "size too large" << std::endl;
                continue;
            }
            if (size == 0) {
                std::cout << "size cannot be zero" << std::endl;
                continue;
            }
            DWORD retSize;
            char* buffer = new char[size];
            memset(buffer, 0, size);
            ioRequest[ioRequestId] = { {}, buffer, (DWORD)size, IO_RESPONSE_IOCTL_READ };
            DeviceIoControl(file, MSG_CODE_READ, NULL, 0, buffer, size, &retSize, &(ioRequest[ioRequestId].overlapped));
            err = GetLastError();
            if (err == ERROR_IO_PENDING) {
                std::cout << "read pending, io request id: " << ioRequestId <<  std::endl;
                ioRequestId++;
            } else {
                if (err != 0)
                    std::cout << "read fail, err code: " << err << std::endl;
                else
                    std::cout << "read completed immediately, size: " << retSize << ", data: " << std::string(buffer, retSize) << std::endl;
                ioRequest.erase(ioRequestId);
                delete[] buffer;
            }
        }
        if (std::regex_search(cmd, matches, ioCtlWriteCmd)) {
            std::string data = matches[1];
            DWORD retSize;
            char* buffer = new char[data.size()];
            memcpy(buffer, data.c_str(), data.size());
            ioRequest[ioRequestId] = { {}, buffer, (DWORD)data.size(), IO_RESPONSE_IOCTL_WRITE };
            DeviceIoControl(file, MSG_CODE_WRITE, buffer, data.size(), NULL, 0, &retSize, &(ioRequest[ioRequestId].overlapped));
            err = GetLastError();
            if (err == ERROR_IO_PENDING) {
                std::cout << "write pending, io request id: " << ioRequestId <<  std::endl;
                ioRequestId++;
            } else {
                if (err != 0)
                    std::cout << "write fail, err code: " << err << std::endl;
                else
                    std::cout << "write completed immediately, bytes written: " << retSize << std::endl;
                ioRequest.erase(ioRequestId);
                delete[] buffer;
            }
        }
        if (std::regex_search(cmd, matches, statCmd)) {
            UINT64 id = std::stoull(matches[1]);
            auto it = ioRequest.find(id);
            if (it == ioRequest.end()) {
                std::cout << "io request not found" << std::endl;
                continue;
            }
            DWORD bytesTransferred = 0;
            BOOL result = GetOverlappedResult(file, &(it->second.overlapped), &bytesTransferred, FALSE);
            if (result) {
                std::cout << "io request completed, bytes transferred: " << bytesTransferred << std::endl;
                if (it->second.type == IO_RESPONSE_READ || it->second.type == IO_RESPONSE_IOCTL_READ)
                    std::cout << "data: " << std::string(it->second.buffer, bytesTransferred) << std::endl;
                delete[] it->second.buffer;
                ioRequest.erase(it);
            } else {
                err = GetLastError();
                if (err == ERROR_IO_INCOMPLETE)
                    std::cout << "io request still pending" << std::endl;
                else
                    std::cout << "get overlapped result fail, err code: " << err << std::endl;
            }
        }
        if (std::regex_search(cmd, matches, cancelCmd)) {
            UINT64 id = std::stoull(matches[1]);
            auto it = ioRequest.find(id);
            if (it == ioRequest.end()) {
                std::cout << "io request not found" << std::endl;
                continue;
            }
            BOOL result = CancelIoEx(file, &(it->second.overlapped));
            err = GetLastError();
            if (result) {
                std::cout << "cancel io request success" << std::endl;
                delete[] it->second.buffer;
                ioRequest.erase(it);
            } else
                std::cout << "cancel io request fail, err code: " << err << std::endl;
        }
    }
    CloseHandle(file);
    file = NULL;
        
    return 0;
}
