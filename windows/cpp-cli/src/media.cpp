#include "media.h"

#include <WinIoCtl.h>

#include <atomic>
#include <cstring>
#include <exception>
#include <limits>
#include <mutex>
#include <vector>

#include "config.h"

namespace testapp {

namespace {

class DenseMemoryMedia final : public BackendCore::Media {
public:
    explicit DenseMemoryMedia(size_t mediaSizeBytes)
        : memory(mediaSizeBytes, 0u)
    {
    }

    bool readLocked(
        UINT64 offset,
        void* buffer,
        UINT32 length) override
    {
        if (length == 0) {
            return true;
        }

        (void)memcpy(
            buffer,
            memory.data() + (size_t)offset,
            length);
        return true;
    }

    bool writeLocked(
        UINT64 offset,
        const void* buffer,
        UINT32 length) override
    {
        if (length == 0) {
            return true;
        }

        (void)memcpy(
            memory.data() + (size_t)offset,
            buffer,
            length);
        return true;
    }

    uint64_t sizeBytes() const override
    {
        return (uint64_t)memory.size();
    }

private:
    std::vector<unsigned char> memory;
};

class SparseFileMedia final : public BackendCore::Media {
public:
    static std::unique_ptr<SparseFileMedia> create(
        ULONG targetId,
        uint64_t mediaSizeBytes,
        std::wstring* outReason)
    {
        const std::wstring path = buildSparseBackingPath(targetId);
        HANDLE backingFile = INVALID_HANDLE_VALUE;
        LARGE_INTEGER fileSize{};
        DWORD bytesReturned = 0;

        if (path.empty()) {
            if (outReason != nullptr) {
                *outReason = L"sparse-dir-create-failed";
            }
            return nullptr;
        }

        backingFile = CreateFileW(
            path.c_str(),
            GENERIC_READ | GENERIC_WRITE,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            nullptr,
            CREATE_ALWAYS,
            FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE,
            nullptr);
        if (backingFile == INVALID_HANDLE_VALUE) {
            if (outReason != nullptr) {
                *outReason = L"sparse-file-open-failed";
            }
            return nullptr;
        }

        if (!DeviceIoControl(
                backingFile,
                FSCTL_SET_SPARSE,
                nullptr,
                0,
                nullptr,
                0,
                &bytesReturned,
                nullptr)) {
            CloseHandle(backingFile);
            if (outReason != nullptr) {
                *outReason = L"sparse-file-mark-failed";
            }
            return nullptr;
        }

        fileSize.QuadPart = (LONGLONG)mediaSizeBytes;
        if (!SetFilePointerEx(backingFile, fileSize, nullptr, FILE_BEGIN) ||
            !SetEndOfFile(backingFile)) {
            CloseHandle(backingFile);
            if (outReason != nullptr) {
                *outReason = L"sparse-file-size-failed";
            }
            return nullptr;
        }

        return std::unique_ptr<SparseFileMedia>(
            new SparseFileMedia(backingFile, mediaSizeBytes, path));
    }

    ~SparseFileMedia() override
    {
        if (backingFile != INVALID_HANDLE_VALUE) {
            CloseHandle(backingFile);
            backingFile = INVALID_HANDLE_VALUE;
        }
    }

    bool readLocked(
        UINT64 offset,
        void* buffer,
        UINT32 length) override
    {
        DWORD bytesRead = 0;
        std::lock_guard<std::mutex> guard(ioLock);

        if (length == 0) {
            return true;
        }

        if (!seekLocked(offset)) {
            return false;
        }
        if (!ReadFile(backingFile, buffer, length, &bytesRead, nullptr)) {
            return false;
        }

        return bytesRead == length;
    }

    bool writeLocked(
        UINT64 offset,
        const void* buffer,
        UINT32 length) override
    {
        DWORD bytesWritten = 0;
        std::lock_guard<std::mutex> guard(ioLock);

        if (length == 0) {
            return true;
        }

        if (!seekLocked(offset)) {
            return false;
        }
        if (!WriteFile(backingFile, buffer, length, &bytesWritten, nullptr)) {
            return false;
        }

        return bytesWritten == length;
    }

    uint64_t sizeBytes() const override
    {
        return mediaSizeBytes;
    }

    const std::wstring& path() const
    {
        return backingPath;
    }

private:
    SparseFileMedia(
        HANDLE fileHandle,
        uint64_t fileSizeBytes,
        std::wstring filePath)
        : backingFile(fileHandle),
          mediaSizeBytes(fileSizeBytes),
          backingPath(std::move(filePath))
    {
    }

    static bool ensureSparseBackingDirectory(
        std::wstring* outDirectory)
    {
        wchar_t tempPath[MAX_PATH];
        DWORD length = GetTempPathW(MAX_PATH, tempPath);
        std::wstring directory;

        if ((length == 0) || (length >= MAX_PATH)) {
            return false;
        }

        directory.assign(tempPath, length);
        while (!directory.empty() &&
               ((directory.back() == L'\\') || (directory.back() == L'/'))) {
            directory.pop_back();
        }
        directory += LR"(\YumeDiskCppCli)";

        if (!CreateDirectoryW(directory.c_str(), nullptr) &&
            (GetLastError() != ERROR_ALREADY_EXISTS)) {
            return false;
        }

        *outDirectory = directory;
        return true;
    }

    static std::wstring buildSparseBackingPath(
        ULONG targetId)
    {
        static std::atomic<UINT64> nonce{1};
        std::wstring directory;

        if (!ensureSparseBackingDirectory(&directory)) {
            return {};
        }

        return directory + L"\\target-" +
            std::to_wstring(targetId) + L"-" +
            std::to_wstring(GetCurrentProcessId()) + L"-" +
            std::to_wstring(nonce.fetch_add(1, std::memory_order_relaxed)) +
            L".bin";
    }

    bool seekLocked(
        UINT64 offset)
    {
        LARGE_INTEGER position{};

        position.QuadPart = (LONGLONG)offset;
        return SetFilePointerEx(backingFile, position, nullptr, FILE_BEGIN) != FALSE;
    }

    HANDLE backingFile = INVALID_HANDLE_VALUE;
    uint64_t mediaSizeBytes = 0;
    std::wstring backingPath;
    std::mutex ioLock;
};

} // namespace

bool CreateManagedDiskMedia(
    const CreateDiskRequest& request,
    CreatedMedia* outMedia,
    std::wstring* outReason)
{
    const MediaMode resolvedMode =
        ResolveMediaMode(request.RequestedMode, request.DiskSizeBytes);

    if ((outMedia == nullptr) || (outReason == nullptr)) {
        return false;
    }

    outMedia->Instance.reset();
    outMedia->Mode = resolvedMode;
    outMedia->BackingDescription.clear();

    if (resolvedMode == MediaMode::Dense) {
        if (request.DiskSizeBytes > kMaxDenseMediaBytes) {
            *outReason = L"dense-limit-exceeded";
            return false;
        }
        if (request.DiskSizeBytes > (uint64_t)std::numeric_limits<size_t>::max()) {
            *outReason = L"dense-size-overflow";
            return false;
        }

        try {
            outMedia->Instance = std::make_unique<DenseMemoryMedia>(
                (size_t)request.DiskSizeBytes);
        } catch (const std::exception&) {
            *outReason = L"dense-allocation-failed";
            return false;
        }

        outMedia->BackingDescription = L"<dense-memory>";
        return true;
    }

    if (resolvedMode == MediaMode::Sparse) {
        auto media = SparseFileMedia::create(
            request.TargetId,
            request.DiskSizeBytes,
            outReason);
        if (media == nullptr) {
            return false;
        }

        outMedia->BackingDescription = media->path();
        outMedia->Instance = std::move(media);
        return true;
    }

    *outReason = L"unsupported-media-mode";
    return false;
}

} // namespace testapp
