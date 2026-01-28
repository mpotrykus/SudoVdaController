#include "../pch.h"
#include "TrayServer.h"

#include "../controller/VirtualDisplayservice.h"
#include "../utils/Logger.h"

#include "TrayWindow.h"
#include "TrayMenuBuilder.h"
#include "PipeProtocol.h"

#include <windows.h>
#include <thread>
#include <atomic>
#include <mutex>
#include <unordered_map>

namespace vdc {

    namespace {

        // Shared running flag for the server lifetime
        static std::atomic<bool> g_running{ false };

    } // anonymous namespace

    // Single global context instance used by WndProc and helpers (external linkage)
    TrayContext g_ctx{};

    int RunTrayServer(const std::wstring& pipeName) {
        LOG_INFO("TrayServer starting...");

        VirtualDisplayService service;
        std::mutex serviceMutex;
        std::unordered_map<UINT, MenuItem> menuMap;

        g_ctx.service = &service;
        g_ctx.serviceMutex = &serviceMutex;
        g_ctx.menuMap = &menuMap;
        g_ctx.pipeName = pipeName;

        g_running.store(true);

        // Create hidden tray window (also registers class & adds tray icon)
        HWND hWnd = CreateTrayWindow(&g_ctx);
        if (!hWnd) {
            LOG_ERROR("Failed to create tray window.");
            return 1;
        }

        // Start pipe listener thread for CLI <-> tray communication
        std::thread pipeThread([&]() {
            RunPipeServerLoop(g_ctx.pipeName, &service, &serviceMutex);
            });

        // Standard message loop
        MSG msg{};
        while (g_running.load()) {
            BOOL res = GetMessageW(&msg, nullptr, 0, 0);
            if (res <= 0) break;
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }

        // Signal shutdown and wait for pipe thread
        g_running.store(false);
        if (pipeThread.joinable())
            pipeThread.join();

        DestroyTrayWindow(hWnd);

        LOG_INFO("TrayServer exiting.");
        return 0;
    }

} // namespace vdc