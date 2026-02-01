#pragma once
#include <string>
#include <vector>
#include <windows.h>
#include "../utils/GuidUtils.h"

namespace vdc {

    struct Topology {
        std::string displayId;
        std::string displayName;
        std::string gdiName;
        std::string edid;
        bool enabled = false;

        bool isVirtual() {
		return vdc::IsGuid(displayId);
        }
    };

    struct DisplayConfig {
        std::string displayId;
        int width = 1920;
        int height = 1080;
        int refreshRateHz = 60000;
        std::vector<Topology> topology;

        bool isVirtual() {
            return vdc::IsGuid(displayId);
        }
    };

    struct Display {
        std::string displayName;
        std::vector<DisplayConfig> configs;
    };

    struct DisplayMap {
        std::vector<Display> displays;
        std::vector<Topology> globalTopology;
        bool mergeDisabledWins = true;
    };

}
