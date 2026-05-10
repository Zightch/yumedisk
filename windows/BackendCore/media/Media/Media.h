#pragma once

#include <Windows.h>

#include <cstdint>

namespace BackendCore {

class Media {
public:
    virtual ~Media() = default;

    virtual bool readLocked(
        UINT64 offset,
        void* buffer,
        UINT32 length) = 0;

    virtual bool writeLocked(
        UINT64 offset,
        const void* buffer,
        UINT32 length) = 0;

    virtual uint64_t sizeBytes() const = 0;
};

} // namespace BackendCore

