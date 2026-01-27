#pragma once
#include <map>
#include <memory>
#include <string>
#include <optional>
#include <vector>
#include "../models/VirtualDisplay.h"
#include "../utils/ConfigStore.h"
#include "VirtualDisplayService.h"

namespace vdc {

    class VirtualDisplayController {
    public:

        VirtualDisplayController();
        ~VirtualDisplayController();

        bool CreateDisplay(const VirtualDisplay& cfg, const std::optional<GUID>& guid = std::nullopt);
        bool RemoveDisplay(const GUID& guid);
        bool SetMode(const GUID& guid, int w, int h, int refreshMilliHz, bool isolatedLayout);
        bool SetPrimary(const GUID& guid);
        bool SetHdr(const GUID& guid, bool enable);
        bool Query(const GUID& guid);

        size_t CountDisplays() const;
        std::vector<std::pair<GUID, std::wstring>> ListDisplays() const;

        bool AddNewDisplayToConfigStore(GUID displayId, VirtualDisplay virtualDisplay, DisplayConfig displayConfig);
        DisplayConfig FindExistingDisplayConfigOrGenerate(const VirtualDisplay& cfg, const std::optional<GUID>& guidOpt);
        DisplayConfig UpdateConfigTopogology(DisplayConfig displayConfig, bool overrideEnabled);

    private:
		std::unique_ptr<VirtualDisplayService> virtualDisplayService_;
        std::map<GUID, std::unique_ptr<VirtualDisplay>> virtualDisplays_;
        std::unique_ptr<ConfigStore> configStore_;
    };

}
