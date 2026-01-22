#pragma once
#include <map>
#include <memory>
#include <string>
#include <optional>
#include <vector>
#include "VdaSession.h"
#include "VirtualDisplaySession.h"
#include "../models/VirtualDisplayConfig.h"
#include "../utils/GuidUtils.h"

namespace vdc {

    struct ControllerResult {
        bool success;
        std::string message;
        std::string json;
    };

    class VirtualDisplayController {
    public:
        VirtualDisplayController();
        ~VirtualDisplayController();

        ControllerResult CreateDisplay(const VirtualDisplayConfig& cfg, const std::optional<GUID>& guid = std::nullopt);
        ControllerResult RemoveDisplay(const GUID& guid);
        ControllerResult SetMode(const GUID& guid, int w, int h, int refreshMilliHz, bool isolatedLayout);
        ControllerResult SetPrimary(const GUID& guid);
        ControllerResult SetHdr(const GUID& guid, bool enable);
        ControllerResult Query(const GUID& guid);

        // Return number of active sessions (for tray lifecycle decisions).
        size_t CountDisplays() const;

        // Return list of (GUID, deviceName) currently managed.
        std::vector<std::pair<GUID, std::wstring>> ListDisplays() const;

    private:
        std::unique_ptr<VdaSession> vda_;
        std::map<GUID, std::unique_ptr<vdisplay::VirtualDisplaySession>, guid_less> sessions_;
    };

} // namespace vdc
