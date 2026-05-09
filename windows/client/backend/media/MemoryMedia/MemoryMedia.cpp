#include "backend/media/MemoryMedia/MemoryMedia.h"

#include <cstring>

namespace clientbackend {

MemoryMedia::MemoryMedia(size_t mediaSizeBytes)
    : memory(mediaSizeBytes, 0u)
{
}

bool MemoryMedia::readLocked(
    UINT64 offset,
    void* buffer,
    UINT32 length)
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

bool MemoryMedia::writeLocked(
    UINT64 offset,
    const void* buffer,
    UINT32 length)
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

uint64_t MemoryMedia::sizeBytes() const
{
    return (uint64_t)memory.size();
}

} // namespace clientbackend
