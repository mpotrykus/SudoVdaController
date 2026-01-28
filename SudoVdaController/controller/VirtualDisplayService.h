#pragma once

#include "../models/VirtualDisplay.h"
#include "../utils/ConfigStore.h"
#include "driver/SudovdaDriver.h"

#include <map>
#include <memory>
#include <string>
#include <optional>
#include <vector>


namespace vdc {

    class VirtualDisplayService {
    public:

        VirtualDisplayService();
        ~VirtualDisplayService();

        bool CreateVirtualDisplay(const VirtualDisplay& cfg, const std::optional<GUID>& guid = std::nullopt);
        bool RemoveVirtualDisplay(const GUID& guid);

        bool SetMode(const std::wstring gdiName, int w, int h, int refreshMilliHz, bool isolatedLayout);
        bool SetPrimary(const std::wstring gdiName);
        bool IsHdrEnabled(const std::wstring gdiName);
        bool SetHdr(const std::wstring gdiName, bool enable);
        bool IsDisplayEnabled(const std::wstring gdiName);
        bool SetDisplayEnabled(const std::wstring gdiName, bool enable);
        bool DisplaySupportsHdr(const std::wstring gdiName);
        bool Query(const std::wstring gdiName);

        size_t CountDisplays() const;
        std::vector<std::pair<GUID, std::wstring>> ListDisplays() const;

        bool AddNewDisplayToConfigStore(GUID displayId, VirtualDisplay virtualDisplay, DisplayConfig displayConfig);
        DisplayConfig FindExistingDisplayConfigOrGenerate(const VirtualDisplay& cfg, const std::optional<GUID>& guidOpt);
        DisplayConfig UpdateConfigTopogology(DisplayConfig displayConfig, bool overrideEnabled);

    private:
        SudovdaDriver* m_sudoVdaDriver;

		std::unique_ptr<VirtualDisplayService> virtualDisplayService_;
        std::map<GUID, std::unique_ptr<VirtualDisplay>> virtualDisplays_;
        std::unique_ptr<ConfigStore> configStore_;

        std::vector<vdc::Topology> GetCurrentTopology(const std::map<GUID, std::unique_ptr<VirtualDisplay>>& virtualDisplays);
    };

}
