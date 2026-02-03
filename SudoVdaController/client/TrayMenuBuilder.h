#pragma once

#include <windows.h>
#include <unordered_map>
#include <string>
#include <mutex>
#include "../models/TrayMenuItem.h"

namespace vdc {

    class VirtualDisplayService;

    // Shared context from TrayServer
    struct TrayContext {
        VirtualDisplayService* service = nullptr;
        std::mutex* serviceMutex = nullptr;
        std::unordered_map<UINT, TrayMenuItem>* menuMap = nullptr;
        std::wstring pipeName;
        HWND hwnd = nullptr;
    };

    // Builds and shows the tray popup menu
    void ShowTrayMenu(HWND hWnd, TrayContext* ctx);

} // namespace vdc
