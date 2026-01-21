#pragma once
#include <windows.h>
#include <optional>

namespace vdisplay {

struct DisplayMode {
    int width;
    int height;
    int refreshRateMilliHz;
};

} // namespace vdisplay

// GUID comparator for std::map
struct guid_less {
    bool operator()(const GUID& a, const GUID& b) const noexcept {
        return memcmp(&a, &b, sizeof(GUID)) < 0;
    }
};
