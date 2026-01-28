#pragma once
#include <string>
#include <map>
#include <memory>
#include <optional>
#include <windows.h>
#include "../models/VirtualDisplayTypes.h"
#include "../models/VirtualDisplay.h"
#include <vector>
#include "../models/DisplayConfig.h"

namespace vdc {

    class DisplayConfigUtils {
    public:
        static std::optional<vdisplay::DisplayMode> GetCurrentModeForDevice(const std::wstring& deviceName);
        static bool ApplyModeForDevice(const std::wstring& deviceName, int w, int h, int refreshMilliHz, bool isolatedLayout);
        static bool MakeDevicePrimary(const std::wstring& deviceName);
        static std::optional<std::wstring> GetMonitorFriendlyNameFromDeviceInstanceId(const std::wstring& deviceInstanceId);
        static std::optional<std::wstring> GetMonitorFriendlyNameForGdiName(const std::wstring& gdiName);
        static bool ApplyTopologyFromStore(const std::map<std::string,bool>& topologyMap);
        static std::optional<std::string> GetMonitorDevicePathForGdiName(const std::wstring& gdiName);
        static std::optional<std::string> GetEdidHexForDeviceInstanceId(const std::wstring& deviceInstanceId);
        static std::optional<std::string> GetWmiKeyForDeviceInstanceId(const std::wstring& deviceInstanceId);

        std::vector<std::pair<GUID, std::wstring>> ListDisplays(const std::map<GUID, std::unique_ptr<VirtualDisplay>, std::less<GUID>>& virtualDisplays) const;
        std::vector<vdc::Topology> GetActiveDisplayTopology(const std::vector<std::pair<GUID,std::wstring>>& virtualDisplays);
    };

} // namespace vdc
