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
        // Retrieve monitor friendly name from the registry EDID for a device instance ID (e.g. "MONITOR\\...\")
        static std::optional<std::wstring> GetMonitorFriendlyNameFromDeviceInstanceId(const std::wstring& deviceInstanceId);
        // Retrieve monitor friendly name for a GDI device name (e.g. "\\.\DISPLAY1") using DisplayConfig APIs
        static std::optional<std::wstring> GetMonitorFriendlyNameForGdiName(const std::wstring& gdiName);
    };

} // namespace vdc
