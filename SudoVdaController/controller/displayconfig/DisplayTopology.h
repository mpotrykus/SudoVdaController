#pragma once

#include "DisplayGeometry.h"
#include <vector>
#include <optional>
#include <string>
#include <windows.h>

namespace vdc {

    // Represents a full snapshot of the current Windows display topology.
    struct DisplayTopologySnapshot {
        std::vector<DISPLAYCONFIG_PATH_INFO> paths;
        std::vector<DISPLAYCONFIG_MODE_INFO> modes;
    };

    // Query the current active display configuration.
    std::optional<DisplayTopologySnapshot> QueryActiveTopology();

    // Convert the DisplayConfig snapshot into DisplayRect structures.
    // Each DisplayRect includes:
    //   - position (x,y)
    //   - width/height
    //   - modeIndex (index into the mode array)
    std::vector<DisplayRect> ExtractRects(const DisplayTopologySnapshot& snap);

    // Apply updated DisplayRect positions/sizes back into the DisplayConfig structures
    // and commit them to Windows.
    bool ApplyTopology(
        DisplayTopologySnapshot& snap,
        const std::vector<DisplayRect>& rects
    );

    // Find adapterId + targetId for a given GDI device name.
    bool FindDisplayIds(
        const std::wstring& gdiName,
        LUID& adapterId,
        uint32_t& targetId
    );

} // namespace vdc
