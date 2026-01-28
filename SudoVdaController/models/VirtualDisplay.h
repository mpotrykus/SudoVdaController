#pragma once
#include <cstdint>
#include <string>

namespace vdc {

	const std::wstring DEFAULT_VIRTUAL_DISPLAY_DEVICE_NAME = L"SudoMakerVDD";

    struct VirtualDisplay {
        std::wstring deviceName = DEFAULT_VIRTUAL_DISPLAY_DEVICE_NAME;
        std::wstring gdiName;
        int width = 1920;
        int height = 1080;
        int refreshRateMilliHz = 60000;
        bool hdr = false;
        bool primary = false;
        uint64_t adapterLuid = 0;
    };

} // namespace vdc
