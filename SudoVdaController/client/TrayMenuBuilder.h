#pragma once

#include <windows.h>
#include <unordered_map>
#include <string>
#include <optional>
#include <mutex>
#include "../models/VirtualDisplay.h"

namespace vdc {

    class VirtualDisplayService;

    // Matches the original enum from your monolithic file
    enum class DisplayAction : int {
        None = 0,
        Details = 1,
        Remove = 2,
        ToggleEnable = 3,
        Add = 4,
        SetPrimary = 5,
        ToggleHdr = 6,
        Custom = 7
    };

    // Represents a single menu entry
    struct MenuItem {
        GUID guid{};
        DisplayAction action = DisplayAction::None;

        // For Add actions
        std::optional<class VirtualDisplay> cfg;

        // For physical displays
        std::wstring gdiName;
        std::wstring physicalLabel;
    };

    // Shared context from TrayServer
    struct TrayContext {
        VirtualDisplayService* service = nullptr;
        std::mutex* serviceMutex = nullptr;
        std::unordered_map<UINT, MenuItem>* menuMap = nullptr;
        std::wstring pipeName;
    };

    // Builds and shows the tray popup menu
    void ShowTrayMenu(HWND hWnd, TrayContext* ctx);

} // namespace vdc
