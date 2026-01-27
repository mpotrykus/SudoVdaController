#include "DisplayTopology.h"
#include "../../utils/Logger.h"

#include <algorithm>

namespace vdc {

    // -----------------------------------------------------------------------------
    // QueryActiveTopology
    // -----------------------------------------------------------------------------
    std::optional<DisplayTopologySnapshot> QueryActiveTopology() {
        UINT32 pathCount = 0;
        UINT32 modeCount = 0;

        if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &pathCount, &modeCount) != ERROR_SUCCESS) {
            LOG_ERROR("GetDisplayConfigBufferSizes failed");
            return std::nullopt;
        }

        DisplayTopologySnapshot snap;
        snap.paths.resize(pathCount);
        snap.modes.resize(modeCount);

        if (QueryDisplayConfig(
            QDC_ONLY_ACTIVE_PATHS,
            &pathCount,
            snap.paths.data(),
            &modeCount,
            snap.modes.data(),
            nullptr) != ERROR_SUCCESS)
        {
            LOG_ERROR("QueryDisplayConfig failed");
            return std::nullopt;
        }

        return snap;
    }

    // -----------------------------------------------------------------------------
    // ExtractRects
    // -----------------------------------------------------------------------------
    std::vector<DisplayRect> ExtractRects(const DisplayTopologySnapshot& snap) {
        std::vector<DisplayRect> rects;

        for (size_t i = 0; i < snap.paths.size(); ++i) {
            const auto& path = snap.paths[i];
            const auto& src = path.sourceInfo;

            // Find matching source mode
            for (size_t j = 0; j < snap.modes.size(); ++j) {
                const auto& mode = snap.modes[j];

                if (mode.infoType == DISPLAYCONFIG_MODE_INFO_TYPE_SOURCE &&
                    mode.adapterId.HighPart == src.adapterId.HighPart &&
                    mode.adapterId.LowPart == src.adapterId.LowPart &&
                    mode.id == src.id)
                {
                    const auto& sm = mode.sourceMode;

                    DisplayRect r;
                    r.pos.x = sm.position.x;
                    r.pos.y = sm.position.y;
                    r.width = sm.width;
                    r.height = sm.height;
                    r.modeIndex = (int)j;

                    rects.push_back(r);
                    break;
                }
            }
        }

        return rects;
    }

    // -----------------------------------------------------------------------------
    // ApplyTopology
    // -----------------------------------------------------------------------------
    bool ApplyTopology(
        DisplayTopologySnapshot& snap,
        const std::vector<DisplayRect>& rects)
    {
        if (rects.size() != snap.paths.size()) {
            LOG_ERROR("Rect count does not match path count");
            return false;
        }

        // Update mode array with new positions/sizes
        for (size_t i = 0; i < rects.size(); ++i) {
            const auto& r = rects[i];
            int idx = r.modeIndex;

            if (idx < 0 || idx >= (int)snap.modes.size())
                continue;

            auto& mode = snap.modes[idx].sourceMode;
            mode.position.x = r.pos.x;
            mode.position.y = r.pos.y;
            mode.width = r.width;
            mode.height = r.height;
        }

        LONG status = SetDisplayConfig(
            (UINT32)snap.paths.size(),
            snap.paths.data(),
            (UINT32)snap.modes.size(),
            snap.modes.data(),
            SDC_APPLY | SDC_USE_SUPPLIED_DISPLAY_CONFIG | SDC_SAVE_TO_DATABASE
        );

        if (status != ERROR_SUCCESS) {
            LOG_ERROR("SetDisplayConfig failed: %d", status);
            return false;
        }

        return true;
    }

    // -----------------------------------------------------------------------------
    // FindDisplayIds
    // -----------------------------------------------------------------------------
    bool FindDisplayIds(
        const std::wstring& gdiName,
        LUID& adapterId,
        uint32_t& targetId)
    {
        UINT32 pathCount = 0;
        UINT32 modeCount = 0;

        if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &pathCount, &modeCount))
            return false;

        std::vector<DISPLAYCONFIG_PATH_INFO> paths(pathCount);
        std::vector<DISPLAYCONFIG_MODE_INFO> modes(modeCount);

        if (QueryDisplayConfig(
            QDC_ONLY_ACTIVE_PATHS,
            &pathCount,
            paths.data(),
            &modeCount,
            modes.data(),
            nullptr))
            return false;

        for (const auto& path : paths) {
            DISPLAYCONFIG_SOURCE_DEVICE_NAME srcName{};
            srcName.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
            srcName.header.size = sizeof(srcName);
            srcName.header.adapterId = path.sourceInfo.adapterId;
            srcName.header.id = path.sourceInfo.id;

            if (DisplayConfigGetDeviceInfo(&srcName.header) != ERROR_SUCCESS)
                continue;

            if (std::wstring_view(srcName.viewGdiDeviceName) == gdiName) {
                adapterId = path.sourceInfo.adapterId;
                targetId = path.targetInfo.id;
                return true;
            }
        }

        return false;
    }

} // namespace vdc
