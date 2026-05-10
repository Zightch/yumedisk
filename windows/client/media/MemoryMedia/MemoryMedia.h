#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "BackendCore.h"

namespace BackendCore {

class MemoryMedia final : public Media {
public:
    explicit MemoryMedia(size_t mediaSizeBytes);

    bool readLocked(
        UINT64 offset,
        void* buffer,
        UINT32 length) override;

    bool writeLocked(
        UINT64 offset,
        const void* buffer,
        UINT32 length) override;

    uint64_t sizeBytes() const override;

private:
    std::vector<unsigned char> memory;
};

} // namespace BackendCore

