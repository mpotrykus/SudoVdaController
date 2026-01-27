#include "../pch.h"
#include "VirtualDisplayService.h"

#include "driver/SudovdaDriver.h"
#include "displayconfig/DisplayTopology.h"
#include "displayconfig/DisplayLayout.h"
#include "displayconfig/DisplayGeometry.h"

#include "../utils/Logger.h"

namespace vdc {

    VirtualDisplayService::VirtualDisplayService() {
        m_driver = new SudovdaDriver();
    }

    VirtualDisplayService::~VirtualDisplayService() {
        delete m_driver;
    }

    // -----------------------------------------------------------------------------
    // CreateVirtualDisplay
    // -----------------------------------------------------------------------------
    std::optional<std::wstring> VirtualDisplayService::CreateVirtualDisplay(
        const char* clientUid,
        const char* clientName,
        uint32_t width,
        uint32_t height,
        float fps,
        const GUID& guid
    ) {
        return m_driver->CreateVirtualDisplay(
            clientUid,
            clientName,
            width,
            height,
            fps,
            guid
        );
    }

    // -----------------------------------------------------------------------------
    // RemoveVirtualDisplay
    // -----------------------------------------------------------------------------
    bool VirtualDisplayService::RemoveVirtualDisplay(const GUID& guid) {
        return m_driver->RemoveVirtualDisplay(guid);
    }

    // -----------------------------------------------------------------------------
    // FindDisplayIds
    // -----------------------------------------------------------------------------
    bool VirtualDisplayService::FindDisplayIds(
        const std::wstring& gdiName,
        LUID& adapterId,
        uint32_t& targetId
    ) {
        return FindDisplayIds(gdiName, adapterId, targetId);
    }

    // -----------------------------------------------------------------------------
    // ChangeDisplaySettings
    // -----------------------------------------------------------------------------
    bool VirtualDisplayService::ChangeDisplaySettings(
        const std::wstring& gdiName,
        int width,
        int height,
        int refreshMilliHz,
        bool isolatedMode
    ) {
        // Step 1: Query current topology
        auto snapOpt = QueryActiveTopology();
        if (!snapOpt) {
            LOG_ERROR("Failed to query display topology");
            return false;
        }

        auto snap = std::move(*snapOpt);

        // Step 2: Extract rects
        auto rects = ExtractRects(snap);

        // Step 3: Find the target display
        int targetIndex = -1;
        for (size_t i = 0; i < snap.paths.size(); ++i) {
            DISPLAYCONFIG_SOURCE_DEVICE_NAME src{};
            src.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
            src.header.size = sizeof(src);
            src.header.adapterId = snap.paths[i].sourceInfo.adapterId;
            src.header.id = snap.paths[i].sourceInfo.id;

            if (DisplayConfigGetDeviceInfo(&src.header) != ERROR_SUCCESS)
                continue;

            if (std::wstring_view(src.viewGdiDeviceName) == gdiName) {
                targetIndex = (int)i;
                break;
            }
        }

        if (targetIndex < 0) {
            LOG_ERROR("Display not found: %ls", gdiName.c_str());
            return false;
        }

        // Step 4: Update mode for target display
        int modeIndex = rects[targetIndex].modeIndex;
        if (modeIndex < 0 || modeIndex >= (int)snap.modes.size()) {
            LOG_ERROR("Invalid mode index");
            return false;
        }

        auto& mode = snap.modes[modeIndex].sourceMode;
        mode.width = width;
        mode.height = height;
        snap.paths[targetIndex].targetInfo.refreshRate = { (UINT32)refreshMilliHz, 1000 };

        // Step 5: If isolated mode, compute new layout
        if (isolatedMode) {
            auto newRects = ComputeIsolatedLayout(rects);

            if (!ApplyTopology(snap, newRects)) {
                LOG_ERROR("Failed to apply isolated layout");
                return false;
            }

            return true;
        }

        // Step 6: Apply normal topology
        if (!ApplyTopology(snap, rects)) {
            LOG_ERROR("Failed to apply display settings");
            return false;
        }

        return true;
    }

} // namespace vdc
