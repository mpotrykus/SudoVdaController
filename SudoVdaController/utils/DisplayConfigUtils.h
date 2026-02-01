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
        bool SetDisplaysEnabled(const std::map<std::string,bool>& edidStates);

        static std::optional<vdisplay::DisplayMode> GetCurrentModeForDevice(const std::wstring& gdiName);
        static bool ApplyModeForDevice(const std::wstring& gdiName, int w, int h, int refreshMilliHz, bool isolatedLayout);
        
        static bool MakeDevicePrimary(const std::wstring& gdiName);
        

        bool ApplyDisplayConfig(std::vector<Topology> requested);

        std::vector<vdc::Topology> GetActiveDisplayTopology(const std::vector<std::pair<GUID,std::wstring>>& virtualDisplays);
        static bool ApplyTopology(const std::map<std::string,bool>& topologyMap);
    };

} // namespace vdc
