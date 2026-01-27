#pragma once
#include <cstdint>
#include <string>

namespace vdc {

    struct VirtualDisplay {
        std::wstring deviceName = L"SudoMakerVDD";
        std::wstring gdiName;
        int width = 1920;
        int height = 1080;
        int refreshRateMilliHz = 60000;
        bool hdr = false;
        bool primary = false;
        uint64_t adapterLuid = 0;
    };

} // namespace vdc
