#pragma once
#include <string>
#include <map>
#include <memory>
#include <optional>
#include <windows.h>
#include "../models/VirtualDisplayTypes.h"
#include <vector>
#include "../models/DisplayConfig.h"

namespace vdc {

    class DisplayConfigUtils {
    public:
        static bool IsDisplayEnabled(const std::string& edidHex);
        bool SetDisplayEnabled(const std::string& edidHexInput, bool enabled);

        static std::optional<vdisplay::DisplayMode> GetCurrentModeForDevice(const std::wstring& gdiName);
        static bool ApplyModeForDevice(const std::wstring& gdiName, int w, int h, int refreshMilliHz, bool isolatedLayout);
        
        static bool MakeDevicePrimary(const std::wstring& gdiName);

        std::vector<vdc::Topology> GetActiveDisplayTopology(const std::vector<std::pair<GUID,std::wstring>>& virtualDisplays);
        bool ApplyDisplayConfig(std::vector<Topology> requested);
    };

} // namespace vdc
