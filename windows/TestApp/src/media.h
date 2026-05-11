#pragma once

#include <memory>
#include <string>

#include "types.h"

namespace testapp {

struct CreatedMedia {
    std::unique_ptr<BackendCore::Media> Instance;
    MediaMode Mode = MediaMode::Auto;
    std::wstring BackingDescription;
};

bool CreateManagedDiskMedia(
    const CreateDiskRequest& request,
    CreatedMedia* outMedia,
    std::wstring* outReason);

} // namespace testapp
