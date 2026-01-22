#pragma once
#include <string>
#include <optional>
#include <windows.h>
#include "../models/VirtualDisplayTypes.h"

namespace vdc {

    class DisplayConfigUtils {
    public:
        static std::optional<vdisplay::DisplayMode> GetCurrentModeForDevice(const std::wstring& deviceName);
        static bool ApplyModeForDevice(const std::wstring& deviceName, int w, int h, int refreshMilliHz, bool isolatedLayout);
        static bool MakeDevicePrimary(const std::wstring& deviceName);
    };

} // namespace vdc
