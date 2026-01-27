#pragma once

#include <string>
#include <optional>
#include <windows.h>
#include <guiddef.h>

namespace vdc {

    class SudovdaDriver;

    // High-level orchestrator for virtual display operations.
    // Delegates:
    //   - driver I/O to SudovdaDriver
    //   - topology queries to DisplayTopology
    //   - layout computation to DisplayLayout
    class VirtualDisplayService {
    public:
        VirtualDisplayService();
        ~VirtualDisplayService();

        // Create a virtual display and return its GDI device name.
        std::optional<std::wstring> CreateVirtualDisplay(
            const char* clientUid,
            const char* clientName,
            uint32_t width,
            uint32_t height,
            float fps,
            const GUID& guid
        );

        // Remove a virtual display by GUID.
        bool RemoveVirtualDisplay(const GUID& guid);

        // Change mode (width/height/refresh) for a display by GDI name.
        // If isolatedMode = true, applies the isolated layout algorithm.
        bool ChangeDisplaySettings(
            const std::wstring& gdiName,
            int width,
            int height,
            int refreshMilliHz,
            bool isolatedMode
        );

        // Find adapter + target IDs for a GDI device name.
        bool FindDisplayIds(
            const std::wstring& gdiName,
            LUID& adapterId,
            uint32_t& targetId
        );

    private:
        SudovdaDriver* m_driver; // owned
    };

} // namespace vdc
