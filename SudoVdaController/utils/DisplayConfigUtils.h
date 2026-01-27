#pragma once
#include <string>
#include <map>
#include <optional>
#include <windows.h>
#include "../models/VirtualDisplayTypes.h"
#include <vector>
#include "../models/DisplayConfig.h"

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
        // Apply topology map (GDI name -> enabled) persisted in ConfigStore. This will disable any
        // currently-active display paths whose GDI name is present and marked as false.
        static bool ApplyTopologyFromStore(const std::map<std::string,bool>& topologyMap);
        // Return the monitor device path (tname.monitorDevicePath) for a given GDI name if available
        static std::optional<std::string> GetMonitorDevicePathForGdiName(const std::wstring& gdiName);
        // Read EDID binary from registry for a device instance id and return as hex string
        static std::optional<std::string> GetEdidHexForDeviceInstanceId(const std::wstring& deviceInstanceId);
        // Build a stable WMI identifier string for a device instance id using WmiMonitorID
        // format: manufacturer-productcode-serial (lowercase)
        static std::optional<std::string> GetWmiKeyForDeviceInstanceId(const std::wstring& deviceInstanceId);

        std::vector<std::pair<GUID, std::wstring>> ListDisplays(std::vector<std::pair<GUID, std::wstring>> virtualDisplays) const;
        std::vector<vdc::Topology> GetActiveDisplayTopology(const std::vector<std::pair<GUID,std::wstring>>& virtualDisplays);
    };

} // namespace vdc
