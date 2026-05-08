#include "media.h"

#include <WinIoCtl.h>

#include <algorithm>
#include <cstring>
#include <exception>
#include <limits>

#include "config.h"

namespace testapp {

namespace {

bool EnsureSparseBackingDirectory(
    std::wstring* out_directory)
{
    wchar_t temp_path[MAX_PATH];
    DWORD length;
    std::wstring directory;

    length = GetTempPathW(MAX_PATH, temp_path);
    if ((length == 0) || (length >= MAX_PATH)) {
        return false;
    }

    directory.assign(temp_path, length);
    while (!directory.empty() &&
           ((directory.back() == L'\\') || (directory.back() == L'/'))) {
        directory.pop_back();
    }
    directory += LR"(\YumeDiskTestApp)";

    if (!CreateDirectoryW(directory.c_str(), nullptr) &&
        (GetLastError() != ERROR_ALREADY_EXISTS)) {
        return false;
    }

    *out_directory = directory;
    return true;
}

std::wstring BuildSparseBackingPath(
    ULONG target_id)
{
    static std::atomic<UINT64> nonce{1};
    std::wstring directory;

    if (!EnsureSparseBackingDirectory(&directory)) {
        return {};
    }

    return directory + L"\\target-" +
           std::to_wstring(target_id) + L"-" +
           std::to_wstring(GetCurrentProcessId()) + L"-" +
           std::to_wstring(nonce.fetch_add(1, std::memory_order_relaxed)) +
           L".bin";
}

bool SeekSparseFileLocked(
    ManagedDisk* disk,
    UINT64 offset)
{
    LARGE_INTEGER position;

    if ((disk == nullptr) || (disk->SparseFile == INVALID_HANDLE_VALUE)) {
        return false;
    }

    position.QuadPart = offset;
    return SetFilePointerEx(disk->SparseFile, position, nullptr, FILE_BEGIN) != FALSE;
}

bool ReadSparseRangeLocked(
    ManagedDisk* disk,
    UINT64 offset,
    void* buffer,
    UINT32 length)
{
    DWORD bytes_read;
    std::lock_guard<std::mutex> io_guard(disk->SparseIoLock);

    if (length == 0) {
        return true;
    }

    if (!SeekSparseFileLocked(disk, offset)) {
        return false;
    }

    bytes_read = 0;
    if (!ReadFile(disk->SparseFile, buffer, length, &bytes_read, nullptr)) {
        return false;
    }

    return bytes_read == length;
}

bool WriteSparseRangeLocked(
    ManagedDisk* disk,
    UINT64 offset,
    const void* buffer,
    UINT32 length)
{
    DWORD bytes_written;
    std::lock_guard<std::mutex> io_guard(disk->SparseIoLock);

    if (length == 0) {
        return true;
    }

    if (!SeekSparseFileLocked(disk, offset)) {
        return false;
    }

    bytes_written = 0;
    if (!WriteFile(disk->SparseFile, buffer, length, &bytes_written, nullptr)) {
        return false;
    }

    return bytes_written == length;
}

bool ReadBackingRangeLocked(
    ManagedDisk* disk,
    UINT64 offset,
    void* buffer,
    UINT32 length)
{
    if (disk->Mode == MediaMode::Dense) {
        (void)memcpy(
            buffer,
            disk->DenseMedium.data() + (size_t)offset,
            length);
        return true;
    }

    if (disk->Mode == MediaMode::Sparse) {
        return ReadSparseRangeLocked(disk, offset, buffer, length);
    }

    return false;
}

bool WriteBackingRangeLocked(
    ManagedDisk* disk,
    UINT64 offset,
    const void* buffer,
    UINT32 length)
{
    if (disk->Mode == MediaMode::Dense) {
        (void)memcpy(
            disk->DenseMedium.data() + (size_t)offset,
            buffer,
            length);
        return true;
    }

    if (disk->Mode == MediaMode::Sparse) {
        return WriteSparseRangeLocked(disk, offset, buffer, length);
    }

    return false;
}

bool InitializeDenseMedia(
    ManagedDisk* disk,
    std::wstring* out_reason)
{
    if (disk->DiskSizeBytes > kMaxDenseMediaBytes) {
        *out_reason = L"dense-limit-exceeded";
        return false;
    }
    if (disk->DiskSizeBytes > (uint64_t)std::numeric_limits<size_t>::max()) {
        *out_reason = L"dense-size-overflow";
        return false;
    }

    try {
        disk->DenseMedium.resize((size_t)disk->DiskSizeBytes, 0u);
    } catch (const std::exception&) {
        *out_reason = L"dense-allocation-failed";
        return false;
    }

    disk->SparseFile = INVALID_HANDLE_VALUE;
    disk->SparseBackingPath.clear();
    return true;
}

bool InitializeSparseMedia(
    ManagedDisk* disk,
    std::wstring* out_reason)
{
    DWORD bytes_returned;
    HANDLE file_handle;
    LARGE_INTEGER file_size;
    const std::wstring path = BuildSparseBackingPath(disk->TargetId);

    if (path.empty()) {
        *out_reason = L"sparse-dir-create-failed";
        return false;
    }

    file_handle = CreateFileW(
        path.c_str(),
        GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr,
        CREATE_ALWAYS,
        FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE,
        nullptr);
    if (file_handle == INVALID_HANDLE_VALUE) {
        *out_reason = L"sparse-file-open-failed";
        return false;
    }

    bytes_returned = 0;
    if (!DeviceIoControl(
            file_handle,
            FSCTL_SET_SPARSE,
            nullptr,
            0,
            nullptr,
            0,
            &bytes_returned,
            nullptr)) {
        CloseHandle(file_handle);
        *out_reason = L"sparse-file-mark-failed";
        return false;
    }

    file_size.QuadPart = disk->DiskSizeBytes;
    if (!SetFilePointerEx(file_handle, file_size, nullptr, FILE_BEGIN) ||
        !SetEndOfFile(file_handle)) {
        CloseHandle(file_handle);
        *out_reason = L"sparse-file-size-failed";
        return false;
    }

    disk->DenseMedium.clear();
    disk->SparseFile = file_handle;
    disk->SparseBackingPath = path;
    return true;
}

} // namespace

size_t CountStagedFragmentsLocked(
    const ManagedDisk* disk)
{
    size_t count;

    count = 0;
    for (const auto& entry : disk->StagedWrites) {
        count += entry.second.Fragments.size();
    }
    return count;
}

bool InitializeManagedDiskMedia(
    ManagedDisk* disk,
    MediaMode requested_mode,
    std::wstring* out_reason)
{
    const MediaMode resolved_mode = ResolveMediaMode(requested_mode, disk->DiskSizeBytes);

    disk->Mode = resolved_mode;
    if (resolved_mode == MediaMode::Dense) {
        return InitializeDenseMedia(disk, out_reason);
    }
    if (resolved_mode == MediaMode::Sparse) {
        return InitializeSparseMedia(disk, out_reason);
    }

    *out_reason = L"unsupported-media-mode";
    return false;
}

void CleanupManagedDiskMedia(
    ManagedDisk* disk)
{
    std::unique_lock<std::shared_mutex> guard(disk->MediaLock);

    disk->StagedWrites.clear();
    disk->DenseMedium.clear();
    disk->DenseMedium.shrink_to_fit();

    if (disk->SparseFile != INVALID_HANDLE_VALUE) {
        CloseHandle(disk->SparseFile);
        disk->SparseFile = INVALID_HANDLE_VALUE;
    }
    disk->SparseBackingPath.clear();
}

AK_STATUS AK_CALL HostReadBytes(
    void* media_ctx,
    const AK_READ_OP* op,
    void* out_buffer,
    UINT32* out_data_length)
{
    ManagedDisk* disk;
    unsigned char* buffer;
    UINT64 request_begin;
    UINT64 request_end;

    disk = static_cast<ManagedDisk*>(media_ctx);
    if ((disk == nullptr) || (op == nullptr) || (out_buffer == nullptr) || (out_data_length == nullptr)) {
        return AK_STATUS_INVALID_PARAMETER;
    }

    if (op->DataLength == 0) {
        *out_data_length = 0;
        return AK_STATUS_SUCCESS;
    }

    request_begin = op->OffsetBytes;
    request_end = request_begin + (UINT64)op->DataLength;
    if ((request_end < request_begin) || (request_end > disk->DiskSizeBytes)) {
        *out_data_length = 0;
        return AK_STATUS_INVALID_PARAMETER;
    }

    buffer = static_cast<unsigned char*>(out_buffer);
    {
        struct OverlaySlice {
            UINT64 Ordinal;
            size_t DestOffset;
            size_t SourceOffset;
            size_t Length;
            const unsigned char* Data;
        };
        std::vector<OverlaySlice> overlays;

        std::shared_lock<std::shared_mutex> guard(disk->MediaLock);
        if (!ReadBackingRangeLocked(disk, request_begin, buffer, op->DataLength)) {
            *out_data_length = 0;
            return AK_STATUS_UNSUCCESSFUL;
        }

        for (const auto& staged_entry : disk->StagedWrites) {
            for (const auto& fragment_entry : staged_entry.second.Fragments) {
                const StagedFragment& fragment = fragment_entry.second;
                UINT64 fragment_begin;
                UINT64 fragment_end;
                UINT64 overlap_begin;
                UINT64 overlap_end;
                OverlaySlice slice;

                fragment_begin = fragment.DiskOffsetBytes;
                fragment_end = fragment_begin + (UINT64)fragment.Data.size();
                if ((fragment_end <= request_begin) || (fragment_begin >= request_end)) {
                    continue;
                }

                overlap_begin = std::max(fragment_begin, request_begin);
                overlap_end = std::min(fragment_end, request_end);
                if (overlap_end <= overlap_begin) {
                    continue;
                }

                slice.Ordinal = fragment.Ordinal;
                slice.DestOffset = (size_t)(overlap_begin - request_begin);
                slice.SourceOffset = (size_t)(overlap_begin - fragment_begin);
                slice.Length = (size_t)(overlap_end - overlap_begin);
                slice.Data = fragment.Data.data();
                overlays.push_back(slice);
            }
        }

        std::sort(
            overlays.begin(),
            overlays.end(),
            [](const OverlaySlice& left, const OverlaySlice& right) {
                return left.Ordinal < right.Ordinal;
            });

        for (const auto& slice : overlays) {
            (void)memcpy(
                buffer + slice.DestOffset,
                slice.Data + slice.SourceOffset,
                slice.Length);
        }
    }

    *out_data_length = op->DataLength;
    return AK_STATUS_SUCCESS;
}

AK_STATUS AK_CALL HostStageWrite(
    void* media_ctx,
    const AK_WRITE_OP* op,
    const void* data_buffer,
    UINT32 data_length)
{
    ManagedDisk* disk;
    UINT64 write_begin;
    UINT64 write_end;

    disk = static_cast<ManagedDisk*>(media_ctx);
    if ((disk == nullptr) || (op == nullptr)) {
        return AK_STATUS_INVALID_PARAMETER;
    }

    if (data_length != op->DataLength) {
        return AK_STATUS_INVALID_PARAMETER;
    }

    write_begin = op->OffsetBytes;
    write_end = write_begin + (UINT64)data_length;
    if ((write_end < write_begin) || (write_end > disk->DiskSizeBytes)) {
        return AK_STATUS_INVALID_PARAMETER;
    }

    {
        std::unique_lock<std::shared_mutex> guard(disk->MediaLock);
        StagedWriteRecord& record = disk->StagedWrites[op->EventId];
        StagedFragment fragment;

        if ((record.TotalSeq != 0) && (record.TotalSeq != op->TotalSeq)) {
            return AK_STATUS_INVALID_PARAMETER;
        }

        record.TotalSeq = op->TotalSeq;
        fragment.Seq = op->Seq;
        fragment.DiskOffsetBytes = op->OffsetBytes;
        fragment.Ordinal = disk->NextStageOrdinal;
        disk->NextStageOrdinal += 1;
        fragment.Data.resize(data_length, 0u);
        if ((data_length != 0) && (data_buffer != nullptr)) {
            (void)memcpy(fragment.Data.data(), data_buffer, data_length);
        }

        record.Fragments[op->Seq] = std::move(fragment);
    }

    return AK_STATUS_SUCCESS;
}

bool ApplyCommittedWrite(
    ManagedDisk* disk,
    UINT64 event_id)
{
    std::unique_lock<std::shared_mutex> guard(disk->MediaLock);
    const auto it = disk->StagedWrites.find(event_id);
    if (it == disk->StagedWrites.end()) {
        return true;
    }

    for (const auto& fragment_entry : it->second.Fragments) {
        const StagedFragment& fragment = fragment_entry.second;
        const UINT64 end_offset = fragment.DiskOffsetBytes + (UINT64)fragment.Data.size();

        if ((end_offset < fragment.DiskOffsetBytes) || (end_offset > disk->DiskSizeBytes)) {
            return false;
        }

        if (!fragment.Data.empty() &&
            !WriteBackingRangeLocked(
                disk,
                fragment.DiskOffsetBytes,
                fragment.Data.data(),
                (UINT32)fragment.Data.size())) {
            return false;
        }
    }

    disk->StagedWrites.erase(it);
    return true;
}

void DiscardStagedWrite(
    ManagedDisk* disk,
    UINT64 event_id)
{
    std::unique_lock<std::shared_mutex> guard(disk->MediaLock);
    disk->StagedWrites.erase(event_id);
}

} // namespace testapp
